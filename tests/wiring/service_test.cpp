#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace bg;
using namespace bg::literals;

using RESULT = TimeSeriesVariable<"RESULT">;

template <template <typename, auto...> typename Field> struct ReferenceServiceBundle {
    Field<REF<TS<i64>>> selected;
};

struct FloatConsumer {
    In<TS<float>> value;

    void evaluate() const noexcept {}
};

TEST_CASE("service declarations retain their client signatures", "[feature:service][feature:wiring]") {
    const auto reference = reference_service<auto(i64)->TS<i64>>{"reference", {"path"}};
    const auto subscription =
        subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>, string)->TS<float>>{
            "subscription", {"key", "path"}};
    const auto audit = service<auto(TS<i64>, TS<float>)->void>{"audit", {"left", "right"}};

    CHECK(reference.signature().parameter_count() == 1);
    CHECK(reference.signature().output() == &TS<i64>::type());

    CHECK(subscription.signature().parameter(0).type == &TS<i64, TimeSeriesGuarantee::final>::type());
    CHECK(subscription.signature().parameter(1).type == &cpp_scalar_type<string>());

    CHECK_FALSE(audit.signature().has_output());
}

TEST_CASE("service declarations reject incompatible signatures", "[feature:service][feature:wiring]") {
    CHECK_THROWS((service<auto() -> double>{"scalar_result"}));
    CHECK_THROWS((reference_service<auto(TS<i64>)->TS<i64>>{"reference", {"value"}}));
    CHECK_THROWS((subscription_service<auto(i64)->TS<i64>>{"subscription", {"key"}}));
    CHECK_THROWS((subscription_service<auto(TS<i64>)->void>{"sink", {"key"}}));
    CHECK_THROWS((subscription_service<auto(TS<i64>)->TS<i64>>{"ordinary_key", {"key"}}));
    CHECK_THROWS((subscription_service<auto(TS<i64, TimeSeriesGuarantee::deduplicated>)->TS<i64>>{
        "deduplicated_key", {"key"}}));
    CHECK_NOTHROW((subscription_service<auto(TS<i64, TimeSeriesGuarantee::const_>)->TS<i64>>{
        "constant_key", {"key"}}));
    CHECK_THROWS((service<auto(i64)->TS<i64>>{"invalid", {"value"}}));
}

TEST_CASE("service implementation registration rejects graph and absent scalar arguments",
          "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto()->TS<i64>>{"lookup"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto implementation = graph("name"_n = "lookup_impl", [&] { return source(); });
    const auto graph_argument =
        GraphArgument{details::GraphWiringStructure{}, std::make_shared<const int>(1)};
    auto catalogue = ServiceCatalogue{};

    CHECK_THROWS_AS(catalogue.register_impl(service, implementation, graph_argument), std::invalid_argument);
    CHECK_THROWS_AS(catalogue.register_impl(service, implementation, std::optional<i64>{}),
                    std::invalid_argument);
}

TEST_CASE("multiservice registration remains attached when its catalogue is moved",
          "[feature:service][feature:multiservice]") {
    const auto clock = reference_service<auto()->TS<i64>>{"clock"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto implementation = graph("name"_n = "clock_impl", [&] {
        clock.implementation_replies(source());
    });

    auto original = ServiceCatalogue{};
    auto registration = original.register_multiservice(implementation);
    auto moved = std::move(original);
    registration.register_impl(clock);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)clock();
        moved.build(wiring);
    }
    wiring.finalize();
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("reference service requirements use resolved types and scalar values for multiplicity",
          "[feature:service][feature:wiring][declaration:generic]") {
    const auto service = reference_service<RESULT, auto(i64)->RESULT>{"lookup", {"path"}};
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto first = service[type<TS<i64>>](1);
    const auto repeated = service[type<TS<i64>>](1);
    const auto other_path = service[type<TS<i64>>](2);

    CHECK(TS<i64>::type().accepts(first.type()));
    CHECK(TS<i64>::type().accepts(repeated.type()));
    CHECK(TS<i64>::type().accepts(other_path.type()));
    CHECK(wiring.inherited_requirements().size() == 2);
}

TEST_CASE("one reference service instance crosses separate inner wiring boundaries",
          "[feature:service][feature:wiring][feature:nested-graph]") {
    const auto service = reference_service<auto(i64)->TS<i64>>{"lookup", {"path"}};
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
    CHECK_THROWS((reference_service<auto()->TSD<i64, REF<TS<i64>>>>{
        "reference_dictionary"}));
    CHECK_THROWS((reference_service<auto()->TSL<REF<TS<i64>>, 1>>{"reference_list"}));
    CHECK_THROWS((reference_service<auto()->TSB<ReferenceServiceBundle>>{"reference_bundle"}));
}

TEST_CASE("a catalogue builds the reference service implementation selected by scalar value",
          "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto(i64)->TS<i64>>{"lookup", {"path"}};
    const auto source = node([](Out<TS<i64>>) {});
    auto selected = i64{};
    const auto first = graph("name"_n = "first_service", [&](i64 path) -> wp<TS<i64>> {
        selected = -path;
        return source();
    });

    const auto second = graph("name"_n = "second_service", [&](i64 path) -> wp<TS<i64>> {
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
    wiring.finalize();
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("finalize with a catalogue drains materialisation added by a late service implementation",
          "[feature:service][feature:wiring][feature:wiring-materialisation]") {
    const auto service = reference_service<auto()->TS<float>>{"late_materialisation"};
    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto float_source = node([](Out<TS<float>>) {});

    const auto consume = node([](In<TS<float>>, Out<TS<float>>) {});
    auto materialisation_builds = 0;
    const auto implementation = graph("name"_n = "late_materialisation_impl", [&] {
        auto &wiring = WiringContext::current().wiring();
        auto deferred = wiring.create_wp<TS<i64>>(integer_source());
        deferred.materialisation_option<TS<float>>(
            [&](wp<TS<i64>>) {
                ++materialisation_builds;
                return float_source();
            },
            0.0);
        return consume(deferred);
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)service();
    }

    wiring.finalize(catalogue);

    CHECK(wiring.is_finalized());
    CHECK(materialisation_builds == 1);
    wiring.finalize();
    CHECK_NOTHROW(CompiledGraph{wiring});
}

template <template <typename, auto...> typename Field> struct MaterialisationServiceBundle {
    Field<TS<i64>> value;
};

TEST_CASE("a committed materialisation keeps its result when a late service adds another form",
          "[feature:service][feature:wiring][feature:wiring-materialisation]") {
    const auto service = reference_service<auto()->TS<bool>>{"materialisation_service"};
    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto consume = node([](In<TS<i64>>, Out<TS<bool>>) {});

    const auto sink = node([](In<TSB<MaterialisationServiceBundle>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto shared = wiring.create_wp<TS<i64>>(integer_source());
    auto expression_builds = 0;
    auto concrete_builds = 0;
    auto expression_result = WiringPoint{};
    shared.materialisation_option<TSB<MaterialisationServiceBundle>>(
        [&](wp<TS<i64>> value) {
            ++expression_builds;
            CHECK_FALSE(wiring.materialisation_required(shared, TS<i64>::type()));
            (void)service();
            expression_result = bundle<TSB<MaterialisationServiceBundle>>("value"_n = value);
            return expression_result;
        },
        0.0);
    shared.materialisation_option<TS<i64>>(
        [&](wp<TS<i64>> value) {
            ++concrete_builds;
            return static_cast<WiringPoint>(value);
        },
        0.0);
    (void)sink(shared);

    const auto implementation = graph("name"_n = "materialisation_service_impl", [&] {
        return consume(shared);
    });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    wiring.finalize(catalogue);

    CHECK(expression_builds == 1);
    CHECK(concrete_builds == 1);
    CHECK(wiring.materialisation_required(shared, TS<i64>::type()));
    CHECK(wiring.expand_materialisation(shared, TSB<MaterialisationServiceBundle>::type()).record() ==
          expression_result.record());

    wiring.finalize(catalogue);

    CHECK(expression_builds == 1);
    CHECK(concrete_builds == 1);
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("catalogue finalization failure closes GraphWiring",
          "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto()->TS<i64>>{"missing_implementation"};
    auto catalogue = ServiceCatalogue{};
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)service();
    }

    CHECK_THROWS_AS(wiring.finalize(catalogue), WiringError);
    CHECK_FALSE(wiring.is_finalized());
    CHECK_THROWS_WITH(wiring.finalize(), Catch::Matchers::ContainsSubstring("failed GraphWiring"));
}

TEST_CASE("catalogue type mappings select a generic reference service implementation",
          "[feature:service][feature:wiring][declaration:generic]") {
    const auto service = reference_service<RESULT, auto()->RESULT>{"generic_lookup"};
    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto floating_source = node([](Out<TS<float>>) {});

    const auto integer = graph("name"_n = "integer_service", [&] { return integer_source(); });

    const auto floating = graph("name"_n = "floating_service", [&] { return floating_source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service[type<TS<i64>>], integer);
    catalogue.register_impl(service[type<TS<float>>], floating);
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        (void)service[type<TS<float>>]();
    }
    catalogue.build(wiring);

    wiring.finalize();
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("service building includes requirements introduced by implementation graphs",
          "[feature:service][feature:wiring]") {
    const auto outer = reference_service<auto()->TS<i64>>{"outer"};
    const auto inner = reference_service<auto()->TS<i64>>{"inner"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto outer_impl = graph("name"_n = "outer_impl", [&]() -> wp<TS<i64>> {
        return inner().as<TS<i64>>();
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
    wiring.finalize();
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("a graph discovers service clients before its late-bound input is provided",
          "[feature:service][feature:wiring][feature:late-bound-wiring]") {
    const auto downstream = reference_service<auto(i64)->TS<i64>>{"downstream", {"key"}};
    const auto consume = node([](In<TS<i64>>) {});

    const auto source = node([](Out<TS<i64>>) {});

    const auto catch_all_implementation = graph("name"_n = "catch_all_impl", [&](wp<TS<i64>> values) {
        consume(values);
        (void)downstream(1);
    });
    const auto downstream_implementation =
        graph("name"_n = "downstream_impl", [&](i64 key) { (void)key; return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(downstream, downstream_implementation);

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto requests = wiring.late_bound_wiring(TS<i64>::type());

    (void)catch_all_implementation(requests);

    wiring.provide_late_bound_wiring(requests, source());
    wiring.finalize(catalogue);

    CHECK(wiring.is_finalized());
    CHECK(wiring.inherited_requirements().size() == 1);
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("service building reports missing and ambiguous implementations", "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto()->TS<i64>>{"lookup"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto implementation = graph("name"_n = "lookup_impl", [&] { return source(); });

    SECTION("missing") {
        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)service();
        }

        CHECK_THROWS_WITH(ServiceCatalogue{}.build(wiring), Catch::Matchers::ContainsSubstring("lookup["));
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

        CHECK_THROWS_WITH(catalogue.build(wiring), Catch::Matchers::ContainsSubstring("lookup["));
    }
}

TEST_CASE("service output mismatch identifies the service and call path", "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto()->TS<i64>>{"lookup"};
    const auto source = node([](Out<TS<float>>) {});

    const auto implementation = graph("name"_n = "lookup_impl", [&] { return source(); });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)service();
    }

    auto diagnostic = std::string{};
    try {
        catalogue.build(wiring);
    } catch (const WiringError &error) {
        diagnostic = error.what();
    }

    CHECK(diagnostic.find("service 'lookup'") != std::string::npos);
    CHECK(diagnostic.find("lookup[") != std::string::npos);
    CHECK(diagnostic.find("expected:") != std::string::npos);
    CHECK(diagnostic.find("received:") != std::string::npos);
}

TEST_CASE("late service failures retain the materialisation consumer", "[feature:service][feature:wiring]") {
    const auto lookup = reference_service<auto()->TS<float>>{"lookup"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto consumer = node<FloatConsumer>();
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto producer_path = WiringPath{nullptr, util::symbol{"producer"}, util::symbol{"create"}};
    auto deferred = [&] {
        const auto producer_context = WiringContext{wiring, producer_path};
        auto point = wiring.create_wp<TS<i64>>(source());
        point.materialisation_option<TS<float>>(
            [lookup](wp<TS<i64>>) { return lookup(); }, 0.0);
        return point;
    }();
    consumer(deferred);

    try {
        wiring.finalize(ServiceCatalogue{});
        FAIL("expected a missing service implementation");
    } catch (const WiringError &error) {
        INFO(error.what());
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == WiringFailureStage::requirement && entry.parameter == util::symbol{"value"} &&
                   !entry.operation.text().empty() && !entry.path.empty();
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == WiringFailureStage::callback &&
                   entry.operation == util::symbol{"materialisation"} &&
                   entry.path == "producer[create]" && !entry.requested_type.empty();
        }));
    }
}

TEST_CASE("nested service failures retain caller and child graph contexts",
          "[feature:service][feature:wiring][feature:nested-graph]") {
    const auto service = reference_service<auto(i64)->TS<i64>>{"nested_lookup", {"value"}};
    const auto running_source = node([](Out<TS<bool>>) {});

    const auto inner = graph("name"_n = "service_child", [&](i64 value) -> wp<TS<i64>> {
        return service(value).as<TS<i64>>();
    });

    const auto outer = graph("name"_n = "service_outer", [&](i64 value) -> wp<REF<TS<i64>>> {
        const auto run = running_source();
        return nested(inner, run, value);
    });
    auto root = GraphWiring{};
    {
        auto context = WiringContext{root};
        (void)outer(42);
    }

    try {
        ServiceCatalogue{}.build(root);
        FAIL("expected a missing nested service implementation");
    } catch (const WiringError &error) {
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == WiringFailureStage::callback &&
                   entry.path.find("service_outer") != std::string::npos;
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == util::symbol{"nested_lookup"} &&
                   entry.path.find("service_child") != std::string::npos;
        }));
    }
}

TEST_CASE("multiservice wiring reports missing and duplicate replies", "[feature:multiservice][feature:wiring]") {
    const auto service = reference_service<auto()->TS<i64>>{"lookup"};
    const auto source = node([](Out<TS<i64>>) {});

    SECTION("missing reply") {
        const auto implementation = graph("name"_n = "missing_reply", [] {});
        auto catalogue = ServiceCatalogue{};
        auto registration = catalogue.register_multiservice(implementation);
        registration.register_impl(service);

        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)service();
        }

        catalogue.build(wiring);
        CHECK_THROWS_WITH(wiring.finalize(),
                          Catch::Matchers::ContainsSubstring("missing implementation_replies"));
        CHECK_FALSE(wiring.is_finalized());
        CHECK_THROWS_WITH(wiring.finalize(), Catch::Matchers::ContainsSubstring("failed GraphWiring"));
    }

    SECTION("duplicate reply") {
        const auto implementation = graph("name"_n = "duplicate_reply", [&] {
            service.implementation_replies(source());
            service.implementation_replies(source());
        });
        auto catalogue = ServiceCatalogue{};
        auto registration = catalogue.register_multiservice(implementation);
        registration.register_impl(service);

        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)service();
        }

        CHECK_THROWS_WITH(catalogue.build(wiring), Catch::Matchers::ContainsSubstring("declared more than once"));
    }
}

TEST_CASE("multiservice wiring rejects an unregistered reply port", "[feature:multiservice][feature:wiring]") {
    const auto claimed = reference_service<auto()->TS<i64>>{"claimed"};
    const auto unclaimed = reference_service<auto()->TS<i64>>{"unclaimed"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto implementation = graph("name"_n = "unregistered_reply", [&] {
        unclaimed.implementation_replies(source());
    });
    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(claimed);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)claimed();
    }

    CHECK_THROWS_WITH(catalogue.build(wiring), Catch::Matchers::ContainsSubstring("unregistered service port"));
}

TEST_CASE("multiservice wiring reports a missing subscription request port",
          "[feature:multiservice][feature:wiring][series:tss]") {
    const auto lookup = subscription_service<auto(TS<i64, TimeSeriesGuarantee::final>)->TS<i64>>{
        "lookup", {"key"}};
    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto implementation = graph("name"_n = "missing_requests", [&] {
        lookup.implementation_replies(source());
    });
    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(lookup);

    auto wiring = GraphWiring{};
    const auto key = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
    {
        const auto context = WiringContext{wiring};
        (void)lookup(key());
    }

    catalogue.build(wiring);
    CHECK_THROWS_WITH(wiring.finalize(),
                      Catch::Matchers::ContainsSubstring("missing implementation_requests"));
}

TEST_CASE("multiservice wiring rejects an unregistered request port", "[feature:multiservice][feature:wiring]") {
    const auto claimed = reference_service<auto()->TS<i64>>{"claimed"};
    const auto unclaimed = service<auto(TS<i64>)->void>{"unclaimed", {"value"}};
    const auto source = node([](Out<TS<i64>>) {});

    const auto implementation = graph("name"_n = "unregistered_request", [&] {
        (void)unclaimed.implementation_requests();
        claimed.implementation_replies(source());
    });
    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(claimed);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)claimed();
    }

    CHECK_THROWS_WITH(catalogue.build(wiring), Catch::Matchers::ContainsSubstring("unregistered service port"));
}

TEST_CASE("multiservice replies do not hide another matching implementation",
          "[feature:service][feature:multiservice][feature:wiring]") {
    const auto first = reference_service<auto()->TS<i64>>{"first"};
    const auto second = reference_service<auto()->TS<i64>>{"second"};
    const auto dependency = reference_service<auto()->TS<i64>>{"dependency"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto multiservice = graph("name"_n = "first_and_dependency", [&] {
        first.implementation_replies(source());
        dependency.implementation_replies(source());
    });

    const auto second_implementation = graph("name"_n = "second_impl", [&] {
        return dependency().as<TS<i64>>();
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(multiservice);
    registration.register_impl(first);
    registration.register_impl(dependency);
    catalogue.register_impl(second, second_implementation);
    catalogue.register_impl(dependency, graph("name"_n = "dependency_impl", [&] { return source(); }));

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)first();
        (void)second();
    }

    CHECK_THROWS_WITH(catalogue.build(wiring), Catch::Matchers::ContainsSubstring("ambiguous implementations"));
}

TEST_CASE("a multiservice registration can build separate root graphs",
          "[feature:service][feature:multiservice][feature:wiring][shape:multiple]") {
    const auto clock = reference_service<auto()->TS<i64>>{"clock"};
    const auto source = node([](Out<TS<i64>>) {});
    auto implementation_wirings = std::size_t{};
    const auto implementation = graph("name"_n = "clock_impl", [&] {
        ++implementation_wirings;
        clock.implementation_replies(source());
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(clock);

    for (auto index = std::size_t{}; index < 2; ++index) {
        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)clock();
        }
        catalogue.build(wiring);
    }

    CHECK(implementation_wirings == 2);
}

TEST_CASE("a multiservice reply does not create a service requirement",
          "[feature:service][feature:multiservice][feature:wiring]") {
    const auto used = reference_service<auto()->TS<i64>>{"used"};
    const auto unused = reference_service<auto()->TS<i64>>{"unused"};
    const auto source = node([](Out<TS<i64>>) {});

    const auto implementation = graph("name"_n = "used_and_unused", [&] {
        used.implementation_replies(source());
        unused.implementation_replies(source());
    });

    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(used);
    registration.register_impl(unused);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)used();
    }
    catalogue.build(wiring);

    CHECK(wiring.inherited_requirements().size() == 1);
}

} // namespace
