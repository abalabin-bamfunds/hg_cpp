#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

using namespace bg;

TEST_CASE("optional inputs remain evaluable through invalidation and revalidation",
          "[feature:validity][validity:optional][transition:invalidation][transition:revalidation]"
          "[feature:operator][series:value][scalar:i64][shape:chain]") {
    const auto choose_left = node([](In<TS<bool>> enabled, In<TS<i64>> value, Out<TS<i64>> output) {
        if (enabled.value()) {
            output.set(value.value());
        } else {
            output.invalidate();
        }
    });

    auto add = operator_<auto(TS_T, TS_T)->TS_T>{"add", {"lhs", "rhs"}};
    add.overload(node([](In<TS<i64>, optional> lhs, In<TS<i64>> rhs, Out<TS<i64>> output) {
        output.set((lhs.valid() ? lhs.value() : i64{}) + rhs.value());
    }));

    auto multiply = operator_<auto(TS_T, TS_T)->TS_T>{"multiply", {"lhs", "rhs"}};
    multiply.overload(node([](In<TS<i64>, optional> lhs, In<TS<i64>, optional> rhs, Out<TS<i64>> output) {
        output.set((lhs.valid() ? lhs.value() : i64{1}) * (rhs.valid() ? rhs.value() : i64{1}));
    }));

    const auto add_graph = [&](wp<TS<bool>> enabled, wp<TS<i64>> lhs,
                               wp<TS<i64>> rhs) -> wp<TS<i64>> {
        return add(opt(choose_left(enabled, lhs)), rhs).as<TS<i64>>();
    };
    const auto multiply_graph = [&](wp<TS<bool>> enabled, wp<TS<i64>> lhs,
                                    wp<TS<i64>> rhs) -> wp<TS<i64>> {
        const auto selected_lhs = choose_left(enabled, lhs);
        return multiply(opt(selected_lhs), opt(rhs)).as<TS<i64>>();
    };

    const auto default_graph = [&](wp<TS<bool>> enabled, wp<TS<i64>> lhs) {
        return operators::default_(opt(choose_left(enabled, lhs)), i64{}).as<TS<i64>>();
    };

    const auto enabled = std::array{false, true, false, true};
    const auto lhs = std::array<i64, 4>{3, 3, 3, 3};
    const auto rhs = std::array<i64, 4>{5, 5, 5, 5};

    CHECK(testing::test_graph(add_graph, enabled, lhs, rhs).equals({5, 8, 5, 8}));
    CHECK(testing::test_graph(multiply_graph, enabled, lhs, rhs).equals({5, 15, 5, 15}));
    CHECK(testing::test_graph(default_graph, enabled, lhs).equals({0, 3, 0, 3}));
}

TEST_CASE("opt does not schedule an optional-input node at graph start",
          "[feature:validity][validity:optional][feature:scheduling][transition:start]"
          "[series:value][scalar:i64][shape:single-node]") {
    auto evaluations = 0;
    const auto copy = node([&](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        ++evaluations;
        output.set(input.valid() ? input.value() : i64{});
    });
    const auto tested = [&](wp<TS<i64>> input) -> wp<TS<i64>> { return copy(opt(input)); };
    const auto no_updates = std::array<std::optional<i64>, 1>{std::nullopt};

    const auto result = testing::test_graph(tested, no_updates);

    CHECK_FALSE(result[0]);
    CHECK(evaluations == 0);
}

TEST_CASE("an optional input node can request evaluation at graph start",
          "[feature:validity][validity:optional][feature:scheduling][transition:start]"
          "[series:value][scalar:i64][shape:single-node]") {
    auto evaluations = 0;
    const auto copy = node(
        eval = [&](In<TS<i64>, optional> input, Out<TS<i64>> output) {
            ++evaluations;
            output.set(input.valid() ? input.value() : i64{});
        },
        schedule_on_start = true);
    const auto tested = [&](wp<TS<i64>> input) -> wp<TS<i64>> { return copy(opt(input)); };
    const auto no_updates = std::array<std::optional<i64>, 1>{std::nullopt};

    const auto result = testing::test_graph(tested, no_updates);

    CHECK(result.equals({0}));
    CHECK(evaluations == 1);
}

TEST_CASE("aggregate optional inputs accept opt-qualified wiring arguments",
          "[feature:validity][validity:optional][feature:wiring][series:value][scalar:i64]"
          "[definition:aggregate][shape:chain]") {
    struct add_optional {
        In<TS<i64>, optional> lhs;
        In<TS<i64>> rhs;
        Out<TS<i64>> output;

        void evaluate() const { output.set((lhs.valid() ? lhs.value() : i64{}) + rhs.value()); }
    };

    const auto choose_left = node([](In<TS<bool>> enabled, In<TS<i64>> value, Out<TS<i64>> output) {
        if (enabled.value()) {
            output.set(value.value());
        } else {
            output.invalidate();
        }
    });
    const auto add = node<add_optional>();
    const auto tested = [&](wp<TS<bool>> enabled, wp<TS<i64>> lhs, wp<TS<i64>> rhs) -> wp<TS<i64>> {
        return add(opt(choose_left(enabled, lhs)), rhs);
    };

    const auto enabled = std::array{false, true, false};
    const auto lhs = std::array<i64, 3>{3, 3, 3};
    const auto rhs = std::array<i64, 3>{5, 5, 5};

    CHECK(testing::test_graph(tested, enabled, lhs, rhs).equals({5, 8, 5}));
}
