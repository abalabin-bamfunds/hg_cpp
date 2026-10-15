#include <bg/graph.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace bg::literals;

struct Add {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

const auto add = bg::node<Add>();

TEST_CASE("test_graph evaluates a node over input ticks",
          "[testing][evaluation][feature:test-graph][transition:update][series:value][scalar:integer]") {
    const auto lhs = std::vector{1.0F, 2.0F, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = bg::testing::test_graph(add, lhs, rhs);

    CHECK(actual.equals<float>({11.0F, 22.0F, 33.0F}));
}

TEST_CASE("test_graph records a profile for a graph function", "[testing][evaluation][feature:profiling]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) { return input; };
    auto profiler = bg::EvaluationProfiler{};

    const auto actual = bg::testing::test_graph(graph, profiler.recorder(), {1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
    const auto root = profiler.root_graph_id();
    REQUIRE(root);
    const auto *summary = profiler.graph(*root);
    REQUIRE(summary);
    REQUIRE(summary->last_updated);
    const auto period = std::chrono::floor<std::chrono::minutes>(*summary->last_updated);
    REQUIRE(profiler.graph_period(*root, period));
    CHECK(profiler.graph_period(*root, period)->evaluations == 3);
}

TEST_CASE("test_graph records a profile for runtime-typed inputs", "[testing][evaluation][feature:profiling]") {
    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    auto ticks = std::vector<std::optional<bg::ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, bg::i64{1});
    ticks.emplace_back(std::in_place, bg::i64{2});
    const auto inputs = std::array{bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks}};
    auto profiler = bg::EvaluationProfiler{};

    const auto actual = bg::testing::test_graph(identity, profiler.recorder(),
                                                 std::span<const bg::testing::TestGraphInput>{inputs});

    CHECK(actual.equals<bg::i64>({1, 2}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("test_graph records a profile for a wiring template", "[testing][evaluation][feature:profiling]") {
    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    const auto ticks = std::vector<bg::i64>{1, 2};
    auto profiler = bg::EvaluationProfiler{};

    const auto actual = bg::testing::test_graph(identity, profiler.recorder(), ticks);

    CHECK(actual.equals<bg::i64>({1, 2}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("test_graph supports skipped input ticks",
          "[testing][evaluation][feature:test-graph][transition:update][series:value][scalar:integer]") {
    const auto add_graph = bg::graph(
        "name"_n = "add_graph",
        [](bg::wp<bg::TS<float>> lhs, bg::wp<bg::TS<float>> rhs) -> bg::wp<bg::TS<float>> { return add(lhs, rhs); });
    const auto lhs = std::vector<std::optional<float>>{1.0F, std::nullopt, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = bg::testing::test_graph(add_graph, lhs, rhs);

    CHECK(actual.equals<float>({11.0F, 21.0F, 33.0F}));
}

struct Increment {
    bg::In<bg::TS<bg::i64>> input;
    int amount;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input + amount); }
};

const auto increment = bg::node<Increment>();

[[nodiscard]] bg::wp<bg::TS<bg::i64>> operator+(bg::wp<bg::TS<bg::i64>> input, int amount) {
    return increment(input, amount);
}

TEST_CASE("test_graph accepts a graph function and reports skipped cycles",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:integer]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) { return input + 1; };

    CHECK(bg::testing::test_graph(graph, {1, 2, std::nullopt, 4}).equals({2, 3, std::nullopt, 5}));
}

TEST_CASE("test_graph captures a runtime-typed output",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) { return bg::wp<>{input}; };

    CHECK(bg::testing::test_graph(graph, {1, 2, std::nullopt, 4}).equals<bg::i64>({1, 2, std::nullopt, 4}));
}

TEST_CASE("test_graph accepts runtime-typed inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity =
        bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    auto ticks = std::vector<std::optional<bg::ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, bg::i64{1});
    ticks.emplace_back(std::nullopt);
    ticks.emplace_back(std::in_place, bg::i64{3});
    const auto inputs = std::array{
        bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks},
    };

    const auto actual = bg::testing::test_graph(identity, std::span<const bg::testing::TestGraphInput>{inputs});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, 3}));
}

TEST_CASE("runtime-typed test_graph inputs can contain only skipped ticks",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity =
        bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    const auto ticks = std::vector<std::optional<bg::ScalarValue<>>>(3);
    const auto inputs = std::array{
        bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks},
    };

    const auto actual = bg::testing::test_graph(identity, std::span<const bg::testing::TestGraphInput>{inputs});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("wiring converts C++ string arguments to the canonical graph type",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:string]") {
    const auto publish = bg::node(
        [](bg::In<bg::TS<bg::i64>>, std::string value, bg::Out<bg::TS<bg::i64>> output) {
            output.set(static_cast<int>(value.size()));
        });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return publish(input, std::string{"text"}); };

    CHECK(bg::testing::test_graph(graph, {1}).equals<bg::i64>({4}));
}

TEST_CASE("wiring converts optional C++ scalar arguments to the canonical graph type",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:integer]") {
    const auto add = bg::node(
        [](bg::In<bg::TS<bg::i64>> input, std::optional<int> amount, bg::Out<bg::TS<bg::i64>> output) {
            output.set(*input + amount.value_or(0));
        });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return add(input, std::optional<int>{2}); };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals<bg::i64>({3, 4}));
}

} // namespace
