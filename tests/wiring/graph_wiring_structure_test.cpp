#include <bg/graph.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace {

using namespace bg;

struct boundary_node {
    static constexpr bool deduplicate_wiring = false;

    Out<TS<i64>> output;

    void evaluate() const {}
};

struct bool_boundary_node {
    static constexpr bool deduplicate_wiring = false;

    Out<TS<bool>> output;

    void evaluate() const {}
};

struct alternate_boundary_node {
    static constexpr bool deduplicate_wiring = false;

    Out<TS<i64>> output;

    void evaluate() const {}
};

struct context_copy_node {
    In<TS<i64>, context> value;
    Out<TS<i64>> output;

    void evaluate() const {}
};

struct add_node {
    In<TS<i64>> left;
    In<TS<i64>> right;
    Out<TS<i64>> output;

    void evaluate() const {}
};

struct multiply_node {
    In<TS<i64>> left;
    In<TS<i64>> right;
    Out<TS<i64>> output;

    void evaluate() const {}
};

struct first_sink_node {
    void evaluate() const {}
};

struct second_sink_node {
    void evaluate() const {}
};

struct branch_node {
    static constexpr bool deduplicate_wiring = false;

    In<TS<i64>> input;
    Out<TS<i64>> output;

    void evaluate() const {}
};

struct opaque_value {
    int value;
};

struct opaque_source_node {
    Scalar<Opaque<opaque_value>> value;
    Out<TS<i64>> output;

    void evaluate() const {}
};

details::GraphWiringStructure add_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<boundary_node>();
    const auto add = node<add_node>();
    const auto first_sink = node<first_sink_node>();
    const auto second_sink = node<second_sink_node>();
    first_sink();
    second_sink();
    const auto x = boundary();
    const auto y = boundary();
    const auto result = add(x, y);
    const auto inputs = std::array<WiringPoint, 2>{x, y};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, inputs, static_cast<WiringPoint>(result));
}

details::GraphWiringStructure renamed_add_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto input = node<boundary_node>();
    const auto operation = node<add_node>();
    const auto first_sink = node<first_sink_node>();
    const auto second_sink = node<second_sink_node>();
    second_sink();
    first_sink();
    const auto lhs = input();
    const auto rhs = input();
    const auto sum = operation(lhs, rhs);
    const auto parameters = std::array<WiringPoint, 2>{lhs, rhs};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, parameters, static_cast<WiringPoint>(sum));
}

details::GraphWiringStructure multiply_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<boundary_node>();
    const auto multiply = node<multiply_node>();
    const auto x = boundary();
    const auto y = boundary();
    const auto result = multiply(x, y);
    const auto inputs = std::array<WiringPoint, 2>{x, y};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, inputs, static_cast<WiringPoint>(result));
}

details::GraphWiringStructure branch_structure(bool shared) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<boundary_node>();
    const auto branch = node<branch_node>();
    const auto add = node<add_node>();
    const auto input = boundary();
    const auto left = branch(input);
    const auto right = shared ? left : branch(input);
    const auto result = add(left, right);
    const auto inputs = std::array<WiringPoint, 1>{input};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, inputs, static_cast<WiringPoint>(result));
}

details::GraphWiringStructure nested_structure() {
    using namespace bg::literals;

    const auto add = node<add_node>();
    const auto inner = graph("name"_n = "inner", [&](wp<TS<i64>> value) { return add(value, value); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto value_boundary = node<boundary_node>();
    const auto run_boundary = node<bool_boundary_node>();
    const auto value = value_boundary();
    const auto run = run_boundary();
    const auto result = nested(inner, run, value);
    const auto inputs = std::array<WiringPoint, 2>{run, value};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, inputs, static_cast<WiringPoint>(result));
}

details::GraphWiringStructure context_structure(bool alternate_provider) {
    using namespace bg::literals;

    const auto first_provider = node<boundary_node>();
    const auto second_provider = node<alternate_boundary_node>();
    const auto copy = node<context_copy_node>();
    auto outer = GraphWiring{};
    const auto outer_context = WiringContext{outer};
    const auto provider =
        alternate_provider ? static_cast<WiringPoint>(second_provider()) : static_cast<WiringPoint>(first_provider());
    const auto scope = with_context("value"_n = provider);
    auto inner = GraphWiring{outer};
    auto result = WiringPoint{};
    {
        const auto inner_context = WiringContext{inner};
        result = copy();
    }

    (void)inner.complete_pending_wiring();
    (void)outer.complete_pending_wiring();
    return details::graph_wiring_structure(inner, std::span<const WiringPoint>{}, result);
}

details::GraphWiringStructure inherited_requirement_structure(const void *identity, bool alternate_provider) {
    const auto first_provider = node<boundary_node>();
    const auto second_provider = node<alternate_boundary_node>();
    const auto copy = node<add_node>();
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto provider =
        alternate_provider ? static_cast<WiringPoint>(second_provider()) : static_cast<WiringPoint>(first_provider());
    const auto requirement_identity = details::NestedBindingIdentity::object(identity);
    const auto requirement =
        wiring.require_inherited_time_series(requirement_identity, util::symbol{"service_input"}, TS<i64>::type());
    const auto result = copy(requirement, requirement);
    wiring.provide_inherited_time_series(requirement_identity, TS<i64>::type(), provider);
    const auto boundaries = std::array<WiringPoint, 1>{provider};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, boundaries, static_cast<WiringPoint>(result));
}

} // namespace

TEST_CASE("graph wiring structure ignores local names and template object lifetimes",
          "[graph_wiring][feature:wiring]") {
    const auto first = add_structure();
    const auto second = renamed_add_structure();

    REQUIRE(first.cacheable());
    REQUIRE(second.cacheable());
    CHECK(first.hash() == second.hash());
    CHECK(first == second);
}

TEST_CASE("graph wiring structure distinguishes leaf definitions", "[graph_wiring][feature:wiring]") {
    const auto add = add_structure();
    const auto multiply = multiply_structure();

    CHECK_FALSE(add == multiply);
}

TEST_CASE("graph wiring structure preserves shared nodes", "[graph_wiring][feature:wiring]") {
    const auto shared = branch_structure(true);
    const auto duplicated = branch_structure(false);

    CHECK_FALSE(shared == duplicated);
}

TEST_CASE("graph structure compares inherited requirement types, not identities or providers",
          "[graph_wiring][feature:wiring]") {
    const auto first_identity = 0;
    const auto second_identity = 0;
    const auto first = inherited_requirement_structure(&first_identity, false);
    const auto same_identity_other_provider = inherited_requirement_structure(&first_identity, true);
    const auto different_identity = inherited_requirement_structure(&second_identity, false);

    CHECK(first == same_identity_other_provider);
    CHECK(first == different_identity);
}

TEST_CASE("service clients have equivalent structure across root wirings",
          "[feature:service][feature:wiring]") {
    const auto audit = service<auto(TS<i64>)->void>{"audit", {"value"}};
    const auto structure = [&] {
        auto wiring = GraphWiring{};
        const auto context = WiringContext{wiring};
        const auto boundary = node<boundary_node>();
        const auto input = boundary();
        audit(input);
        const auto inputs = std::array<WiringPoint, 1>{input};

        (void)wiring.complete_pending_wiring();
        return details::graph_wiring_structure(wiring, inputs, std::nullopt);
    };

    const auto first = structure();
    const auto second = structure();

    REQUIRE(first.cacheable());
    REQUIRE(second.cacheable());
    CHECK(first.hash() == second.hash());
    CHECK(first == second);
}

TEST_CASE("an unhashable wired scalar disables graph structure caching", "[graph_wiring][feature:wiring]") {
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto source = bg::node<opaque_source_node>();
    const auto value = bg::ScalarValue<bg::Opaque<opaque_value>>{opaque_value{1}};
    const auto result = source(value);
    (void)wiring.complete_pending_wiring(result);

    const auto structure = bg::details::graph_wiring_structure(wiring, std::span<const bg::WiringPoint>{},
                                                               static_cast<bg::WiringPoint>(result));

    CHECK_FALSE(structure.cacheable());
    CHECK(structure.hash() == 0);
}

TEST_CASE("generated nested nodes use their inner graph as definition identity",
          "[graph_wiring][feature:wiring][feature:nested-graph][shape:nested]") {
    const auto first = nested_structure();
    const auto second = nested_structure();

    REQUIRE(first.cacheable());
    CHECK(first == second);
}

TEST_CASE("graph wiring structure excludes the provider identity of inherited context",
          "[feature:context-time-series][feature:wiring]") {
    const auto first = context_structure(false);
    const auto second = context_structure(true);

    REQUIRE(first.cacheable());
    REQUIRE(second.cacheable());
    CHECK(first.hash() == second.hash());
    CHECK(first == second);
}
