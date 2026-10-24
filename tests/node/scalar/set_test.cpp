#include <bg/bg.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace {

using namespace bg;

TEST_CASE("a scalar set propagates through a value time series",
          "[node][time_series][scalar][set][scalar:set][feature:state]") {
    const auto source = node([](Scalar<Set<i64>> value, Out<TS<Set<i64>>> output) { output.set_value(value); });

    const auto sum = node([](In<TS<Set<i64>>> input) {
        auto result = i64{};
        input.value_view().for_each([&](const auto value) { result += value.value(); });
        return result;
    });

    const auto input = ScalarValue<Set<i64>>{std::array<i64, 2>{1, 2}};

    const auto graph = [&] { return sum(source(input)); };

    CHECK(bg::run_graph(graph).equals<i64>({3}));
}

TEST_CASE("a scalar set output accepts a C++ range",
          "[node][time_series][scalar][set][scalar:set][feature:state]") {
    const auto source = node([](Out<TS<Set<i64>>> output) { output.set(std::array<i64, 3>{1, 1, 2}); });
    const auto size = node([](In<TS<Set<i64>>> input) { return static_cast<i64>(input.value_view().size()); });
    const auto graph = [&] { return size(source()); };

    CHECK(bg::run_graph(graph).equals<i64>({2}));
}

TEST_CASE("a scalar set output rejects an incompatible scalar range",
          "[node][time_series][scalar][set][scalar:set][feature:state][transition:update]") {
    struct invalid_range_output {
        In<TS<i64>> action;
        Out<TS<Set<i64>>> output;

        void evaluate() const {
            if (*action == 0) {
                output.set(std::array<i64, 2>{1, 2});
                return;
            }

            CHECK_THROWS_AS(output.set(std::array{1.0, 2.0}), std::invalid_argument);
        }
    };

    const auto source = node<invalid_range_output>();
    const auto size = node([](In<TS<Set<i64>>> input) { return static_cast<i64>(input.value_view().size()); });
    const auto graph = [&](wp<TS<i64>> action) { return size(source(action)); };

    CHECK(bg::run_graph(graph, {0, 1}).equals<i64>({2, std::nullopt}));
}

struct inspect_scalar_set_state {
    State<Set<i64>> values;
    Out<TS<i64>> output;

    void start() const {
        (void)values->insert(1);
        (void)values->insert(2);
        output.set(static_cast<i64>(values->size()));
    }

    void evaluate() const {}
};

TEST_CASE("scalar set state is available during node start",
          "[node][state][scalar][set][scalar:set][feature:state][transition:start]") {
    const auto inspect = node<inspect_scalar_set_state>();

    const auto graph = [&] { return inspect(); };

    CHECK(bg::run_graph(graph).equals<i64>({2}));
}

struct inspect_scalar_set {
    Scalar<Set<i64>> values;
    Out<TS<i64>> output;

    void start() const {
        auto result = i64{};
        values.for_each([&](const auto value) { result += value.value(); });
        output.set(result);
    }

    void evaluate() const {}
};

TEST_CASE("a scalar set is available from packed graph scalar storage",
          "[node][scalar][set][scalar:set][feature:state]") {
    const auto inspect = node<inspect_scalar_set>();

    const auto input = ScalarValue<Set<i64>>{std::array<i64, 2>{1, 2}};

    const auto graph = [&] { return inspect(input); };

    CHECK(bg::run_graph(graph).equals<i64>({3}));
}

} // namespace
