#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
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
