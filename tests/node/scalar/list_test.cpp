#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace {

using namespace bg;

TEST_CASE("a scalar list propagates through a value time series",
          "[node][time_series][scalar][list][scalar:list][feature:state]") {
    const auto source = node([](Scalar<List<i64>> value, Out<TS<List<i64>>> output) { output.set_value(value); });

    const auto sum = node([](In<TS<List<i64>>> input) {
        const auto values = input.value_view();
        return values[0].value() + values[1].value();
    });

    const auto input = ScalarValue<List<i64>>{std::array<i64, 2>{1, 2}};

    const auto graph = [&] { return sum(source(input)); };

    CHECK(testing::test_graph(graph).equals<i64>({3}));
}

TEST_CASE("a scalar list output accepts a C++ range",
          "[node][time_series][scalar][list][scalar:list][feature:state]") {
    const auto source = node([](Out<TS<List<i64>>> output) { output.set(std::array<i64, 3>{1, 2, 3}); });
    const auto sum = node([](In<TS<List<i64>>> input) {
        const auto values = input.value_view();
        return values[0].value() + values[1].value() + values[2].value();
    });
    const auto graph = [&] { return sum(source()); };

    CHECK(testing::test_graph(graph).equals<i64>({6}));
}

TEST_CASE("a scalar list output rejects an incompatible scalar range",
          "[node][time_series][scalar][list][scalar:list][feature:state][transition:update]") {
    struct invalid_range_output {
        In<TS<i64>> action;
        Out<TS<List<i64>>> output;

        void evaluate() const {
            if (*action == 0) {
                output.set(std::array<i64, 2>{1, 2});
                return;
            }

            CHECK_THROWS_AS(output.set(std::array{1.0, 2.0}), std::invalid_argument);
        }
    };

    const auto source = node<invalid_range_output>();
    const auto size = node([](In<TS<List<i64>>> input) { return static_cast<i64>(input.value_view().size()); });
    const auto graph = [&](wp<TS<i64>> action) { return size(source(action)); };

    CHECK(testing::test_graph(graph, {0, 1}).equals<i64>({2, std::nullopt}));
}

TEST_CASE("a scalar list output owns nested borrowed values on replacement",
          "[node][time_series][scalar][list][scalar:list][scalar:string][feature:state]") {
    const auto source = node([](Out<TS<List<List<string>>>> output) {
        auto previous = ScalarValue<List<string>>{std::array<std::string_view, 1>{"previous"}};
        output.set(std::array{previous.view()});
        const auto previous_output = output.value();
        output.set(std::array{previous_output[0]});
        CHECK(output.value()[0][0].value() == "previous");

        {
            auto replacement = ScalarValue<List<string>>{std::array<std::string_view, 1>{"replacement"}};
            output.set(std::array{replacement.view()});
        }

        CHECK(output.value().size() == 1);
        CHECK(output.value()[0][0].value() == "replacement");
    });

    const auto inspect = node([](In<TS<List<List<string>>>> input) { return input.value_view()[0][0].value(); });
    CHECK(testing::test_graph([&] { return inspect(source()); }).equals<string>({"replacement"}));
}

struct deduplicated_empty_list_output {
    In<TS<i64>> action;
    Out<TS<List<i64>, TimeSeriesGuarantee::deduplicated>> output;

    void start() const { output.set(std::array<i64, 0>{}); }
    void evaluate() const {
        if (*action == 2) {
            output.invalidate();
            return;
        }

        output.set(std::array<i64, 0>{});
    }
};

TEST_CASE("a deduplicated empty list output suppresses equal ranges and revalidates",
          "[node][time_series][scalar][list][feature:scalar-collection][series:value][scalar:list]"
          "[guarantee:deduplicated]"
          "[transition:start][transition:update][transition:invalidation][transition:revalidation]") {
    const auto source = node<deduplicated_empty_list_output>();
    const auto inspect = node([](In<TS<List<i64>>, optional> input) { return input.valid(); });
    const auto graph = [&](wp<TS<i64>> action) { return inspect(source(action)); };

    CHECK(testing::test_graph(graph, {0, 1, 2, 0}).equals<bool>({true, std::nullopt, false, true}));
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
    const auto source = node([](Scalar<List<i64>> value, Out<TS<List<i64>>> output) { output.set_value(value); });

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
        CHECK(runtime.node(0).output_last_updated().valid());
    }

    CHECK(memory.allocations() == 0);
}

} // namespace
