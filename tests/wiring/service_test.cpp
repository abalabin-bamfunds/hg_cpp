#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace {

using namespace bg;
using namespace bg::literals;

using RESULT = TimeSeriesVariable<"RESULT">;

TEST_CASE("service declarations retain their client signatures", "[feature:service][feature:wiring]") {
    const auto reference = reference_service<auto(std::int64_t)->TS<std::int64_t>>{"reference", {"path"}};
    const auto subscription =
        subscription_service<auto(TS<std::int64_t>, string)->TS<float>>{"subscription", {"key", "path"}};
    const auto compute = compute_service<auto(TS<std::int64_t>, TS<float>)->void>{"compute", {"left", "right"}};

    CHECK(reference.kind() == ServiceKind::reference);
    CHECK(reference.signature().parameter_count() == 1);
    CHECK(reference.signature().output() == &TS<std::int64_t>::type());

    CHECK(subscription.kind() == ServiceKind::subscription);
    CHECK(subscription.signature().parameter(0).type == &TS<std::int64_t>::type());
    CHECK(subscription.signature().parameter(1).type == &cpp_scalar_type<string>());

    CHECK(compute.kind() == ServiceKind::compute);
    CHECK_FALSE(compute.signature().has_output());
}

TEST_CASE("service declarations reject signatures for another service kind", "[feature:service][feature:wiring]") {
    CHECK_THROWS((reference_service<auto(TS<std::int64_t>)->TS<std::int64_t>>{"reference", {"value"}}));
    CHECK_THROWS((subscription_service<auto(std::int64_t)->TS<std::int64_t>>{"subscription", {"key"}}));
    CHECK_THROWS((compute_service<auto(std::int64_t)->TS<std::int64_t>>{"compute", {"value"}}));
}

TEST_CASE("reference service requirements use resolved types and scalar values for multiplicity",
          "[feature:service][feature:wiring][declaration:generic]") {
    const auto service = reference_service<RESULT, auto(std::int64_t)->RESULT>{"lookup", {"path"}};
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto first = service[type<TS<std::int64_t>>](1);
    const auto repeated = service[type<TS<std::int64_t>>](1);
    const auto other_path = service[type<TS<std::int64_t>>](2);

    CHECK(TS<std::int64_t>::type().accepts(first.type()));
    CHECK(TS<std::int64_t>::type().accepts(repeated.type()));
    CHECK(TS<std::int64_t>::type().accepts(other_path.type()));
    CHECK(wiring.inherited_requirements().size() == 2);
}

TEST_CASE("one reference service instance crosses separate inner wiring boundaries",
          "[feature:service][feature:wiring][feature:nested-graph]") {
    const auto service = reference_service<auto(std::int64_t)->TS<std::int64_t>>{"lookup", {"path"}};
    auto root = GraphWiring{};
    auto first_inner = GraphWiring{root};
    auto second_inner = GraphWiring{root};

    {
        const auto context = WiringContext{first_inner};
        (void)service(1);
    }
    {
        const auto context = WiringContext{second_inner};
        (void)service(1);
    }

    CHECK(first_inner.inherited_requirements().size() == 1);
    CHECK(second_inner.inherited_requirements().size() == 1);
    CHECK(root.inherited_requirements().size() == 1);
}

TEST_CASE("resolved service signatures reject contained references", "[feature:service][feature:wiring][series:tsd]") {
    CHECK_THROWS((reference_service<auto()->TSD<std::int64_t, REF<TS<std::int64_t>>>>{
        "reference_dictionary"}));
}

TEST_CASE("a catalogue builds the reference service implementation selected by scalar value",
          "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto(std::int64_t)->TS<std::int64_t>>{"lookup", {"path"}};
    const auto source = node([](Out<TS<std::int64_t>>) {});
    auto selected = std::int64_t{};
    const auto first = graph("name"_n = "first_service", [&](std::int64_t path) -> wp<TS<std::int64_t>> {
        selected = -path;
        return source();
    });
    const auto second = graph("name"_n = "second_service", [&](std::int64_t path) -> wp<TS<std::int64_t>> {
        selected = path;
        return source();
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, first, "path"_n = 1);
    catalogue.register_impl(service, second, "path"_n = 2);
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        (void)service(2);
    }
    catalogue.build(wiring);

    CHECK(selected == 2);
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("catalogue type mappings select a generic reference service implementation",
          "[feature:service][feature:wiring][declaration:generic]") {
    const auto service = reference_service<RESULT, auto()->RESULT>{"generic_lookup"};
    const auto integer_source = node([](Out<TS<std::int64_t>>) {});
    const auto floating_source = node([](Out<TS<float>>) {});
    const auto integer = graph("name"_n = "integer_service", [&] { return integer_source(); });
    const auto floating = graph("name"_n = "floating_service", [&] { return floating_source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service[type<TS<std::int64_t>>], integer);
    catalogue.register_impl(service[type<TS<float>>], floating);
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        (void)service[type<TS<float>>]();
    }
    catalogue.build(wiring);

    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("service building includes requirements introduced by implementation graphs",
          "[feature:service][feature:wiring]") {
    const auto outer = reference_service<auto()->TS<std::int64_t>>{"outer"};
    const auto inner = reference_service<auto()->TS<std::int64_t>>{"inner"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto outer_impl = graph("name"_n = "outer_impl", [&]() -> wp<TS<std::int64_t>> {
        return inner().as<TS<std::int64_t>>();
    });
    const auto inner_impl = graph("name"_n = "inner_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(outer, outer_impl);
    catalogue.register_impl(inner, inner_impl);
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        (void)outer();
    }
    catalogue.build(wiring);

    CHECK(wiring.inherited_requirements().size() == 2);
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("service building reports missing and ambiguous implementations", "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto()->TS<std::int64_t>>{"lookup"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto implementation = graph("name"_n = "lookup_impl", [&] { return source(); });

    SECTION("missing") {
        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)service();
        }

        CHECK_THROWS_WITH(ServiceCatalogue{}.build(wiring), "service 'lookup' has no matching implementation");
    }

    SECTION("ambiguous") {
        auto catalogue = ServiceCatalogue{};
        catalogue.register_impl(service, implementation);
        catalogue.register_impl(service, implementation);
        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)service();
        }

        CHECK_THROWS_WITH(catalogue.build(wiring), "service 'lookup' has ambiguous implementations");
    }
}

TEST_CASE("subscription and compute clients fail until their runtime bindings are implemented",
          "[feature:service][feature:wiring]") {
    const auto subscription =
        subscription_service<auto(TS<std::int64_t>)->TS<float>>{"subscription", {"key"}};
    const auto compute = compute_service<auto(TS<std::int64_t>)->TS<float>>{"compute", {"value"}};
    const auto source = node([](Out<TS<std::int64_t>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    CHECK_THROWS_WITH(subscription(source()), "subscription and compute service clients are not implemented");
    CHECK_THROWS_WITH(compute(source()), "subscription and compute service clients are not implemented");
}

} // namespace
