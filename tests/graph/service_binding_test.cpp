#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

using namespace bg;
using namespace bg::literals;

TEST_CASE("compiled root graph contains its service binding provider",
          "[feature:service][feature:graph-compilation][shape:nested]") {
    const auto clock = reference_service<auto()->TS<i64, TimeSeriesGuarantee::final>>{"clock"};
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<float>>{
        "lookup", {"key"}};

    const auto clock_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(1); });
    const auto clock_implementation = graph("name"_n = "clock_impl", [&] { return clock_source(); });
    const auto lookup_response = node([](In<TSS<i64>> requests,
                                         Out<TSD<i64, TS<float>>> output) {
        requests.for_each_added([&](const auto &key) { output[key].set(static_cast<float>(*key)); });
    });
    const auto consume = node([](In<TS<float>>) {});
    const auto lookup_implementation = graph("name"_n = "lookup_impl", [&](wp<TSS<i64>> requests) {
        return lookup_response(requests);
    });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(clock, clock_implementation);
    catalogue.register_impl(lookup, lookup_implementation);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        consume(lookup(clock().as<TS<i64, TimeSeriesGuarantee::final>>()));
        catalogue.build(wiring);
        wiring.finalize();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    CHECK(compiled.service_binding_count() == 1);
}

TEST_CASE("service request ordering points at the service client after input materialization",
          "[feature:service][feature:graph-compilation]") {
    const auto audit = service<auto(TS<i64>)->void>{"audit", {"value"}};
    const auto receive = node([](In<TSD<service_client_id_t, TS<i64>>>) {});
    const auto implementation = graph(
        "name"_n = "audit_impl",
        [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) { receive(requests); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(audit, implementation);

    const auto make_reference = node([](Out<REF<TS<i64>>>) {});
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto reference = make_reference();
        audit(reference);
        catalogue.build(wiring);
        wiring.finalize();
    }

    const auto &binding = wiring.service_binding_requirements().front();
    REQUIRE(binding.provider.has_output());

    auto found_service_client = false;
    for (const auto consumer_index : binding.consumers) {
        const auto &consumer = wiring.nodes()[wiring.node_index(consumer_index)];
        if (consumer.path().name.text() == "service_client") {
            found_service_client = true;
        }
    }
    CHECK(found_service_client);
}

} // namespace
