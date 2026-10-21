#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

using namespace bg;
using namespace bg::literals;

static_assert(wp<TS<i64>, InputValidity::required>::validity == required);
static_assert(wp<TS<i64>, InputValidity::optional>::validity == optional);

TEST_CASE("an optional input reports validity and reads its value",
          "[feature:validity][validity:optional][transition:invalidation][transition:revalidation]"
          "[series:value][scalar:i64][shape:single-node]") {

    const auto value_or_missing = node([](std::optional<i64> input) { return input.value_or(-1); });

    CHECK(testing::test_graph(value_or_missing, {3, literal::INVALID, 5}).equals({3, -1, 5}));
}

TEST_CASE("opt does not schedule an optional-input node at graph start",
          "[feature:validity][validity:optional][feature:scheduling][transition:start]"
          "[series:value][scalar:i64][shape:single-node]") {
    auto evaluations = 0;

    const auto copy = node([&](In<TS<i64>, optional> input) {
        ++evaluations;
        return input.valid() ? input.value() : i64{};
    });

    const auto tested = [&](wp<TS<i64>> input) { return copy(opt(input)); };
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
        eval = [&](In<TS<i64>, optional> input) {
            ++evaluations;
            return input.valid() ? input.value() : i64{};
        },
        schedule_on_start = true);

    const auto tested = [&](wp<TS<i64>> input) { return copy(opt(input)); };
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

    const auto copy = node([](i64 value) { return value; });

    const auto add = node<add_optional>();

    const auto graph = bg::graph("name"_n = "add_optional", [&](wp<TS<i64>> lhs, wp<TS<i64>> rhs) {
        return add(opt(copy(lhs)), rhs);
    });

    CHECK(testing::test_graph(graph, {literal::INVALID, 3, literal::INVALID}, {5, 5, 5}).equals({5, 8, 5}));
}

TEST_CASE("optional inputs remain evaluable through invalidation and revalidation",
          "[feature:validity][validity:optional][transition:invalidation][transition:revalidation]"
          "[feature:operator][series:value][scalar:i64][shape:chain]") {

    const auto copy = node([](i64 value) { return value; });

    auto add = operator_<auto(TS_T, TS_T)->TS_T>{"add", {"lhs", "rhs"}};
    add.overload(node([](In<TS<i64>, optional> lhs, In<TS<i64>> rhs) {
        return (lhs.valid() ? lhs.value() : i64{}) + rhs.value();
    }));

    const auto add_graph = [&](wp<TS<i64>> lhs, wp<TS<i64>> rhs) -> wp<TS<i64>> {
        return add(opt(copy(lhs)), rhs).as<TS<i64>>();
    };

    CHECK(testing::test_graph(add_graph, {literal::INVALID, 3, literal::INVALID, 3}, {5, 5, 5, 5})
              .equals({5, 8, 5, 8}));
}

TEST_CASE("optional multiplication uses one for each invalid operand",
          "[feature:validity][validity:optional][transition:invalidation][transition:revalidation]"
          "[feature:operator][series:value][scalar:i64][shape:chain]") {

    const auto copy = node([](i64 value) { return value; });

    auto multiply = operator_<auto(TS_T, TS_T)->TS_T>{"multiply", {"lhs", "rhs"}};
    multiply.overload(node([](In<TS<i64>, optional> lhs, In<TS<i64>, optional> rhs) {
        return (lhs.valid() ? lhs.value() : i64{1}) * (rhs.valid() ? rhs.value() : i64{1});
    }));

    const auto graph = [&](wp<TS<i64>> lhs, wp<TS<i64>> rhs) {
        return multiply(opt(copy(lhs)), opt(copy(rhs))).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        graph,
        {literal::INVALID, 3, literal::INVALID, 3, literal::INVALID, literal::INVALID, 3},
        {5, 5, literal::INVALID, literal::INVALID, 5, literal::INVALID, 5});

    CHECK(actual.equals({5, 15, 1, 3, 5, 1, 15}));
}

TEST_CASE("default uses its fallback while the optional input is invalid",
          "[feature:validity][validity:optional][transition:invalidation][transition:revalidation]"
          "[feature:operator][series:value][scalar:i64][shape:chain]") {

    const auto copy = node([](i64 value) { return value; });

    const auto graph = [&](wp<TS<i64>> input) {
        return operators::default_(opt(copy(input)), i64{}).as<TS<i64>>();
    };

    CHECK(testing::test_graph(graph, {literal::INVALID, 3, literal::INVALID, 3}).equals({0, 3, 0, 3}));
}
