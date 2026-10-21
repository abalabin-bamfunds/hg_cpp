#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>

namespace {

using namespace bg;

TEST_CASE("a scalar list propagates through a value time series",
          "[node][time_series][scalar][list][scalar:list][feature:state]") {
    const auto source = node([](List<i64> value) { return value; });

    const auto sum = node([](In<TS<List<i64>>> input) {
        const auto values = input.value_view();
        return values[0].value() + values[1].value();
    });

    const auto input = ScalarValue<List<i64>>{std::array<i64, 2>{1, 2}};

    const auto graph = [&] { return sum(source(input)); };

    CHECK(testing::test_graph(graph).equals<i64>({3}));
}

struct inspect_scalar_list_state {
    State<List<i64>> values;
    Out<TS<i64>> output;

    void start() const {
        values->append(1);
        values->append(2);
        output.set((*values)[0] + (*values)[1]);
    }

    void evaluate() const {}
};

TEST_CASE("scalar list state is available during node start",
          "[node][state][scalar][list][scalar:list][feature:state][transition:start]") {
    const auto inspect = node<inspect_scalar_list_state>();

    const auto graph = [&] { return inspect(); };

    CHECK(testing::test_graph(graph).equals<i64>({3}));
}

struct inspect_scalar_list {
    Scalar<List<i64>> values;
    Out<TS<i64>> output;

    void start() const { output.set(values[0].value() + values[1].value()); }

    void evaluate() const {}
};

TEST_CASE("a scalar list is available from packed graph scalar storage",
          "[node][scalar][list][scalar:list][feature:state]") {
    const auto inspect = node<inspect_scalar_list>();

    const auto input = ScalarValue<List<i64>>{std::array<i64, 2>{1, 2}};

    const auto graph = [&] { return inspect(input); };

    CHECK(testing::test_graph(graph).equals<i64>({3}));
}

TEST_CASE("a runtime graph releases scalar list time-series storage",
          "[node][time_series][scalar][list][scalar:list][allocation][feature:state]") {
    const auto source = node([](List<i64> value) { return value; });

    const auto input = ScalarValue<List<i64>>{std::array<i64, 2>{1, 2}};
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)source(input);
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    {
        auto runtime_global_state = GlobalState{allocator{memory}};
        auto runtime = RuntimeGraph{graph, allocator{memory}, runtime_global_state};
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

} // namespace
