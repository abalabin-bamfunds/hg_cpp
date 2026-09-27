#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
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
