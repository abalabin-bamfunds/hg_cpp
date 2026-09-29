#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/service.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace bg;

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

} // namespace
