#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <utility>

namespace {

using namespace bg;

struct runtime_input_add {
    In<TS<i64>> left;
    In<TS<i64>> right;
    Out<TS<i64>> output;

    void evaluate() const {}
};

details::GraphWiringStructure runtime_input_structure(bool swap_inputs) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto left = wiring.runtime_input(TS<i64>::type());
    const auto right = wiring.runtime_input(TS<i64>::type());
    const auto add = node<runtime_input_add>();
    const auto result = swap_inputs ? add(right, left) : add(left, right);

    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           static_cast<WiringPoint>(result));
}

std::pair<details::GraphWiringStructure, bool> direct_runtime_input_structure(std::size_t selected_input) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto first = wiring.runtime_input(TS<i64>::type());
    const auto second = wiring.runtime_input(TS<i64>::type());
    const auto result = selected_input == 0 ? first : second;

    return {details::graph_wiring_structure(wiring, std::span<const WiringPoint>{}, result), wiring.nodes().empty()};
}

} // namespace

TEST_CASE("runtime inputs are distinct external graph endpoints",
          "[wiring][feature:runtime-input][series:value][shape:multiple]") {
    const auto first = runtime_input_structure(false);
    const auto same = runtime_input_structure(false);
    const auto swapped = runtime_input_structure(true);

    CHECK(first == same);
    CHECK_FALSE(first == swapped);
}

TEST_CASE("a runtime input can be the graph result without producer nodes",
          "[wiring][feature:runtime-input][series:value][shape:source]") {
    const auto [first, first_has_no_nodes] = direct_runtime_input_structure(0);
    const auto [same, same_has_no_nodes] = direct_runtime_input_structure(0);
    const auto [second, second_has_no_nodes] = direct_runtime_input_structure(1);

    CHECK(first_has_no_nodes);
    CHECK(same_has_no_nodes);
    CHECK(second_has_no_nodes);
    CHECK(first.cacheable());
    CHECK(first == same);
    CHECK(first.hash() == same.hash());
    CHECK_FALSE(first == second);
}

TEST_CASE("runtime inputs reject collections and value-to-reference materialization",
          "[wiring][feature:runtime-input][series:value][binding:value-to-reference]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    CHECK_THROWS_WITH(wiring.runtime_input(TSD<i64, TS<i64>>::type()),
                      "runtime inputs currently support TS<V> and REF<TS<V>> only");
    CHECK_THROWS_WITH(wiring.runtime_input(TS<i64, TimeSeriesGuarantee::final>::type()),
                      "a rebindable runtime input requires an ordinary guarantee");
    const auto &incompatible_runtime_type = resolve_time_series_runtime_type(TS<double>::type());
    CHECK_THROWS_WITH(wiring.runtime_input(TS<i64>::type(), incompatible_runtime_type),
                      "a runtime input representation is incompatible with its time series type");

    const auto input = wiring.runtime_input(TS<i64>::type());
    CHECK_THROWS_WITH(wiring.materialize(input, REF<TS<i64>>::type()),
                      "a runtime input cannot create an owning reference output");
}
