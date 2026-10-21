#include <bg/graph.hpp>
#include <bg/graph/details/nested.hpp>
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

bool nested_reference_and_borrowed_bodies_equivalent(bool swapped_inputs) {
    auto nested_wiring = GraphWiring{};
    const auto nested_context = WiringContext{nested_wiring};
    const auto &reference_type = type_cast<TimeSeriesReferenceType>(REF<TS<i64>>::type());
    const auto nested_left = details::wire_nested_input(reference_type);
    const auto nested_right = details::wire_nested_input(reference_type);

    const auto nested_add = node<add_node>();
    const auto nested_result = nested_add(nested_left, nested_right);
    const auto nested_concrete_result = nested_wiring.complete_pending_wiring(nested_result);
    const auto nested_inputs = std::array{nested_left, nested_right};

    auto compact_wiring = GraphWiring{};
    const auto compact_context = WiringContext{compact_wiring};
    const auto compact_left = compact_wiring.borrowed_input(TS<i64>::type());
    const auto compact_right = compact_wiring.borrowed_input(TS<i64>::type());

    const auto compact_add = node<add_node>();
    const auto compact_result = swapped_inputs ? compact_add(compact_right, compact_left)
                                               : compact_add(compact_left, compact_right);
    const auto compact_concrete_result = compact_wiring.complete_pending_wiring(compact_result);
    const auto compact_inputs = std::array{compact_left, compact_right};
    const auto input_types = std::array<const TimeSeriesType *, 2>{&TS<i64>::type(), &TS<i64>::type()};

    return details::equivalent_wiring_bodies(nested_wiring, nested_inputs, nested_concrete_result, compact_wiring,
                                             compact_inputs, compact_concrete_result, input_types);
}

details::GraphWiringStructure ordinary_borrowed_structure(bool swapped_inputs, bool unrelated_input) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto left = wiring.borrowed_input(TS<i64>::type());
    const auto right = wiring.borrowed_input(TS<i64>::type());
    if (unrelated_input) {
        (void)wiring.borrowed_input(TS<bool>::type());
    }

    const auto add = node<add_node>();
    const auto result = swapped_inputs ? add(right, left) : add(left, right);
    const auto concrete_result = wiring.complete_pending_wiring(result);

    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{}, concrete_result);
}

details::GraphWiringStructure direct_borrowed_result(std::size_t result_index, bool declared_inputs) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto first = wiring.borrowed_input(TS<i64>::type());
    const auto second = wiring.borrowed_input(TS<i64>::type());
    if (declared_inputs) {
        const auto inputs = std::array{first, second};
        return details::graph_wiring_structure(wiring, inputs, result_index == 0 ? first : second);
    }
    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           result_index == 0 ? first : second);
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
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto source = node<opaque_source_node>();
    const auto value = ScalarValue<Opaque<opaque_value>>{opaque_value{1}};
    const auto result = source(value);
    (void)wiring.complete_pending_wiring(result);

    const auto structure = details::graph_wiring_structure(
        wiring, std::span<const WiringPoint>{}, static_cast<WiringPoint>(result));

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

TEST_CASE("nested reference boundaries and borrowed values have equivalent bodies",
          "[graph_wiring][feature:borrowed-input][series:value][binding:reference-to-value][shape:multiple]") {
    CHECK(nested_reference_and_borrowed_bodies_equivalent(false));
    CHECK_FALSE(nested_reference_and_borrowed_bodies_equivalent(true));
}

TEST_CASE("ordinary graph snapshots include borrowed input slots",
          "[graph_wiring][feature:borrowed-input][series:value][shape:multiple]") {
    const auto first = ordinary_borrowed_structure(false, false);
    const auto same = ordinary_borrowed_structure(false, false);
    const auto swapped = ordinary_borrowed_structure(true, false);
    const auto with_unrelated_input = ordinary_borrowed_structure(false, true);

    REQUIRE(first.cacheable());
    CHECK(first == same);
    CHECK_FALSE(first == swapped);
    CHECK_FALSE(first == with_unrelated_input);
}

TEST_CASE("a borrowed input can be a declared boundary and graph result",
          "[graph_wiring][feature:borrowed-input][series:value][shape:source]") {
    const auto first = direct_borrowed_result(0, true);
    const auto same = direct_borrowed_result(0, true);
    const auto second = direct_borrowed_result(1, true);

    CHECK(first.cacheable());
    CHECK(first.boundary_count() == 2);
    CHECK(first.result_type() == &TS<i64>::type());
    CHECK(first == same);
    CHECK_FALSE(first == second);
}

TEST_CASE("an undeclared borrowed input can be the graph result",
          "[graph_wiring][feature:borrowed-input][series:value][shape:source]") {
    const auto first = direct_borrowed_result(0, false);
    const auto same = direct_borrowed_result(0, false);
    const auto second = direct_borrowed_result(1, false);

    CHECK(first.cacheable());
    CHECK(first.boundary_count() == 0);
    CHECK(first.result_type() == &TS<i64>::type());
    CHECK(first == same);
    CHECK_FALSE(first == second);
}
