#include <bg/bg.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/testing/test_graph.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace bg;

TEST_CASE("a scalar dictionary propagates through a value time series",
          "[node][time_series][scalar][dict][scalar:dict][feature:state]") {
    const auto source = node(
        [](Scalar<Dict<i64, i64>> value, Out<TS<Dict<i64, i64>>> output) { output.set_value(value); });

    const auto sum = node([](In<TS<Dict<i64, i64>>> input) {
        auto result = i64{};
        input.value_view().for_each([&](const auto, const auto value) { result += value.value(); });
        return result;
    });

    const auto input = ScalarValue<Dict<i64, i64>>{
        std::array{std::pair<i64, i64>{1, 2}, std::pair<i64, i64>{3, 4}}};

    const auto graph = [&] { return sum(source(input)); };

    CHECK(testing::test_graph(graph).equals<i64>({6}));
}

TEST_CASE("a scalar dictionary output accepts a C++ range",
          "[node][time_series][scalar][dict][scalar:dict][feature:state]") {
    const auto source = node([](Out<TS<Dict<i64, i64>>> output) {
        output.set(std::array{std::pair<i64, i64>{1, 2}, std::pair<i64, i64>{3, 4}});
    });
    const auto sum = node([](In<TS<Dict<i64, i64>>> input) {
        auto result = i64{};
        input.value_view().for_each([&](const auto, const auto value) { result += value.value(); });
        return result;
    });
    const auto graph = [&] { return sum(source()); };

    CHECK(testing::test_graph(graph).equals<i64>({6}));
}

TEST_CASE("a scalar dictionary output rejects incompatible items and rolls back duplicate keys",
          "[node][time_series][scalar][dict][scalar:dict][feature:state][feature:scalar-storage]") {
    struct invalid_range_output {
        In<TS<i64>> action;
        Out<TS<Dict<i64, i64>>> output;

        void evaluate() const {
            if (*action == 0) {
                output.set(std::array{std::pair<i64, i64>{1, 10}});
                return;
            }

            CHECK_THROWS_AS(output.set(std::array{std::pair<i64, double>{2, 2.5}}), std::invalid_argument);
        }
    };
    const auto invalid_source = node<invalid_range_output>();
    const auto size = node([](In<TS<Dict<i64, i64>>> input) { return static_cast<i64>(input.value_view().size()); });
    const auto invalid_graph = [&](wp<TS<i64>> action) { return size(invalid_source(action)); };
    CHECK(testing::test_graph(invalid_graph, {0, 1}).equals<i64>({1, std::nullopt}));

    struct duplicate_range_output {
        In<TS<i64>> action;
        Out<TS<Dict<i64, i64>>> output;

        void evaluate() const {
            if (*action == 0) {
                output.set(std::array{std::pair<i64, i64>{1, 10}});
                return;
            }

            const auto duplicates = std::array{std::pair<i64, i64>{2, 20}, std::pair<i64, i64>{2, 30}};
            CHECK_THROWS_AS(output.set(duplicates), std::invalid_argument);
            const auto values = output.value();
            REQUIRE(values.size() == 1);
            CHECK(values.at(1).value() == 10);
        }
    };
    const auto duplicate_source = node<duplicate_range_output>();
    const auto duplicate_graph = [&](wp<TS<i64>> action) { return size(duplicate_source(action)); };
    CHECK(testing::test_graph(duplicate_graph, {0, 1}).equals<i64>({1, std::nullopt}));

    auto memory = counting_memory_resource{};
    const auto source = node([&](Out<TS<Dict<string, string>>> output) {
        output.set(std::array{std::pair{std::string_view{"kept"}, std::string_view{"original"}}});
        const auto allocations_before = memory.allocations();
        const auto duplicates = std::array{std::pair{std::string_view{"candidate"}, std::string_view{"first"}},
                                           std::pair{std::string_view{"candidate"}, std::string_view{"second"}}};

        CHECK_THROWS_AS(output.set(duplicates), std::invalid_argument);
        CHECK(memory.allocations() == allocations_before);

        const auto values = output.value();
        REQUIRE(values.size() == 1);
        REQUIRE(values.contains("kept"));
        CHECK(values.at("kept").value() == "original");
    });

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        source();
    }
    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    {
        auto runtime = RuntimeGraph{graph, allocator{memory}, global_state};
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

struct inspect_scalar_dict_state {
    State<Dict<i64, i64>> values;
    Out<TS<i64>> output;

    void start() const {
        values->insert_or_assign(1, 2);
        values->insert_or_assign(3, 4);
        output.set(values->at(1) + values->at(3));
    }

    void evaluate() const {}
};

TEST_CASE("scalar dictionary state is available during node start",
          "[node][state][scalar][dict][scalar:dict][feature:state][transition:start]") {
    const auto inspect = node<inspect_scalar_dict_state>();

    const auto graph = [&] { return inspect(); };

    CHECK(testing::test_graph(graph).equals<i64>({6}));
}

struct inspect_scalar_dict {
    Scalar<Dict<i64, i64>> values;
    Out<TS<i64>> output;

    void start() const {
        auto result = i64{};
        values.for_each([&](const auto, const auto value) { result += value.value(); });
        output.set(result);
    }

    void evaluate() const {}
};

TEST_CASE("a scalar dictionary is available from packed graph scalar storage",
          "[node][scalar][dict][scalar:dict][feature:state]") {
    const auto inspect = node<inspect_scalar_dict>();

    const auto input = ScalarValue<Dict<i64, i64>>{
        std::array{std::pair<i64, i64>{1, 2}, std::pair<i64, i64>{3, 4}}};

    const auto graph = [&] { return inspect(input); };

    CHECK(testing::test_graph(graph).equals<i64>({6}));
}

} // namespace
