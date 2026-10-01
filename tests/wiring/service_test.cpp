#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph.hpp>
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
    Field<REF<TS<std::int64_t>>> selected;
};

TEST_CASE("service declarations retain their client signatures", "[feature:service][feature:wiring]") {
    const auto reference = reference_service<auto(std::int64_t)->TS<std::int64_t>>{"reference", {"path"}};
    const auto subscription =
        subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::final>, string)->TS<float>>{
            "subscription", {"key", "path"}};
    const auto audit = service<auto(TS<std::int64_t>, TS<float>)->void>{"audit", {"left", "right"}};

    CHECK(reference.signature().parameter_count() == 1);
    CHECK(reference.signature().output() == &TS<std::int64_t>::type());

    CHECK(subscription.signature().parameter(0).type == &TS<std::int64_t, TimeSeriesGuarantee::final>::type());
    CHECK(subscription.signature().parameter(1).type == &cpp_scalar_type<string>());

    CHECK_FALSE(audit.signature().has_output());
}

TEST_CASE("service declarations reject incompatible signatures", "[feature:service][feature:wiring]") {
    CHECK_THROWS((reference_service<auto(TS<std::int64_t>)->TS<std::int64_t>>{"reference", {"value"}}));
    CHECK_THROWS((subscription_service<auto(std::int64_t)->TS<std::int64_t>>{"subscription", {"key"}}));
    CHECK_THROWS((subscription_service<auto(TS<std::int64_t>)->void>{"sink", {"key"}}));
    CHECK_THROWS((subscription_service<auto(TS<std::int64_t>)->TS<std::int64_t>>{"ordinary_key", {"key"}}));
    CHECK_THROWS((subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::deduplicated>)->TS<std::int64_t>>{
        "deduplicated_key", {"key"}}));
    CHECK_NOTHROW((subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::const_>)->TS<std::int64_t>>{
        "constant_key", {"key"}}));
    CHECK_THROWS((service<auto(std::int64_t)->TS<std::int64_t>>{"invalid", {"value"}}));
}

TEST_CASE("service implementation registration rejects graph and absent scalar arguments",
          "[feature:service][feature:wiring]") {
    const auto service = reference_service<auto()->TS<std::int64_t>>{"lookup"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto implementation = graph("name"_n = "lookup_impl", [&] { return source(); });
    const auto graph_argument =
        GraphArgument{details::GraphWiringStructure{}, std::make_shared<const int>(1)};
    auto catalogue = ServiceCatalogue{};

    CHECK_THROWS_AS(catalogue.register_impl(service, implementation, graph_argument), std::invalid_argument);
    CHECK_THROWS_AS(catalogue.register_impl(service, implementation, std::optional<std::int64_t>{}),
                    std::invalid_argument);
}

TEST_CASE("multiservice registration remains attached when its catalogue is moved",
          "[feature:service][feature:multiservice]") {
    const auto clock = reference_service<auto()->TS<std::int64_t>>{"clock"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
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
    CHECK_NOTHROW(CompiledGraph{wiring});
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
    CHECK_THROWS((reference_service<auto()->TSL<REF<TS<std::int64_t>>, 1>>{"reference_list"}));
    CHECK_THROWS((reference_service<auto()->TSB<ReferenceServiceBundle>>{"reference_bundle"}));
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
    const auto service = reference_service<auto()->TS<std::int64_t>>{"lookup"};
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

TEST_CASE("multiservice wiring reports missing and duplicate replies", "[feature:multiservice][feature:wiring]") {
    const auto service = reference_service<auto()->TS<std::int64_t>>{"lookup"};
    const auto source = node([](Out<TS<std::int64_t>>) {});

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

        CHECK_THROWS_WITH(catalogue.build(wiring),
                          Catch::Matchers::ContainsSubstring("missing implementation_replies"));
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
    const auto claimed = reference_service<auto()->TS<std::int64_t>>{"claimed"};
    const auto unclaimed = reference_service<auto()->TS<std::int64_t>>{"unclaimed"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
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
    const auto lookup = subscription_service<auto(TS<std::int64_t, TimeSeriesGuarantee::final>)->TS<std::int64_t>>{
        "lookup", {"key"}};
    const auto source = node([](Out<TSD<std::int64_t, TS<std::int64_t>>>) {});
    const auto implementation = graph("name"_n = "missing_requests", [&] {
        lookup.implementation_replies(source());
    });
    auto catalogue = ServiceCatalogue{};
    auto registration = catalogue.register_multiservice(implementation);
    registration.register_impl(lookup);

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto key = node([](Out<TS<std::int64_t, TimeSeriesGuarantee::final>>) {});
        (void)lookup(key());
    }

    CHECK_THROWS_WITH(catalogue.build(wiring), Catch::Matchers::ContainsSubstring("missing implementation_requests"));
}

TEST_CASE("multiservice wiring rejects an unregistered request port", "[feature:multiservice][feature:wiring]") {
    const auto claimed = reference_service<auto()->TS<std::int64_t>>{"claimed"};
    const auto unclaimed = service<auto(TS<std::int64_t>)->void>{"unclaimed", {"value"}};
    const auto source = node([](Out<TS<std::int64_t>>) {});
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
    const auto first = reference_service<auto()->TS<std::int64_t>>{"first"};
    const auto second = reference_service<auto()->TS<std::int64_t>>{"second"};
    const auto dependency = reference_service<auto()->TS<std::int64_t>>{"dependency"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto multiservice = graph("name"_n = "first_and_dependency", [&] {
        first.implementation_replies(source());
        dependency.implementation_replies(source());
    });
    const auto second_implementation = graph("name"_n = "second_impl", [&] {
        return dependency().as<TS<std::int64_t>>();
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
    const auto clock = reference_service<auto()->TS<std::int64_t>>{"clock"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
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
    const auto used = reference_service<auto()->TS<std::int64_t>>{"used"};
    const auto unused = reference_service<auto()->TS<std::int64_t>>{"unused"};
    const auto source = node([](Out<TS<std::int64_t>>) {});
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
