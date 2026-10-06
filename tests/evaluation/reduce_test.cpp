#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/reduce.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace {

using namespace bg;
using namespace bg::literals;

const auto dictionary_from_two_values = node([](In<TS<i64>> left, In<TS<i64>> right,
                                                 In<TS<bool>> include_left, In<TS<bool>> include_right,
                                                 Out<TSD<std::int64_t, TS<i64>>> output) {
    if (*include_left) {
        output[1].set(*left);
    } else {
        output.erase(1);
    }
    if (*include_right) {
        output[2].set(*right);
    } else {
        output.erase(2);
    }
});

const auto dictionary_from_three_values = node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<i64>> third,
                                                   Out<TSD<std::int64_t, TS<i64>>> output) {
    output[1].set(*first);
    output[2].set(*second);
    output[3].set(*third);
});

const auto dictionary_from_validity_state = node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<i64>> state,
                                                     Out<TSD<std::int64_t, TS<i64>>> output) {
    switch (*state) {
    case 0:
        output[1].set(*first);
        output[2].set(*second);
        output[2].invalidate();
        break;
    case 1:
        output[1].invalidate();
        output[2].invalidate();
        break;
    case 2:
        output.clear();
        break;
    }
});

const auto add_values = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) {
    output.set(*left + *right);
});

const auto add_graph = graph("name"_n = "add_values_for_reduce", [](wp<TS<i64>> left, wp<TS<i64>> right) {
    return add_values(left, right);
});

const auto add_values_and_context = node([](In<TS<i64>> left, In<TS<i64>> right,
                                            In<TS<i64>, "factor"_n, context> factor,
                                            Out<TS<i64>> output) {
    output.set(*left + *right + *factor);
});

const auto context_add_graph = graph("name"_n = "context_add_for_reduce", [](wp<TS<i64>> left,
                                                                               wp<TS<i64>> right) {
    return add_values_and_context(left, right);
});

const auto namespace_scope_reduce_with_default = graph(
    "name"_n = "namespace_scope_reduce_with_default", [](wp<TS<i64>> first, wp<TS<i64>> second,
                                                           wp<TS<i64>> state) -> wp<TS<i64>> {
        return wp<TS<i64>>{
            reduce(add_graph, dictionary_from_validity_state(first, second, state), "default"_n = std::int64_t{20})};
    });

TEST_CASE("reduce combines all valid TSD items through nested reducer graphs",
          "[feature:reduce][feature:nested-graph][series:tsd][shape:nested]") {
    const auto tested = [](wp<TS<i64>> left, wp<TS<i64>> right, wp<TS<bool>> include_left,
                           wp<TS<bool>> include_right) {
        return reduce(add_graph, dictionary_from_two_values(left, right, include_left, include_right));
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2, 3}, std::array{4, 5, 6},
                                            std::array{true, true, true}, std::array{true, true, true});

    CHECK(actual.equals<i64>({5, 7, 9}));
}

TEST_CASE("reduce carries an odd item to the next level of its tree",
          "[feature:reduce][feature:nested-graph][series:tsd][shape:nested]") {
    const auto tested = [](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> third) {
        return reduce(add_graph, dictionary_from_three_values(first, second, third));
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2}, std::array{4, 5}, std::array{7, 8});

    CHECK(actual.equals<i64>({12, 15}));
}

TEST_CASE("reduce adapts a generic lambda to the TSD item type",
          "[feature:reduce][feature:operator][series:tsd][definition:function]") {
    const auto tested = [](wp<TS<i64>> left, wp<TS<i64>> right, wp<TS<bool>> include_left,
                           wp<TS<bool>> include_right) {
        return reduce([](auto first, auto second) { return add_values(first, second); },
                      dictionary_from_two_values(left, right, include_left, include_right));
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2}, std::array{4, 5},
                                            std::array{true, true}, std::array{true, true});

    CHECK(actual.equals<i64>({5, 7}));
}

TEST_CASE("reduce carries inherited context references into its reducer graphs",
          "[feature:reduce][feature:context-time-series][feature:nested-graph]") {
    const auto tested = [](wp<TS<i64>> left, wp<TS<i64>> right, wp<TS<bool>> include_left,
                           wp<TS<bool>> include_right, wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        return reduce(context_add_graph, dictionary_from_two_values(left, right, include_left, include_right));
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2}, std::array{4, 5},
                                            std::array{true, true}, std::array{true, true},
                                            std::array{10, 20});

    CHECK(actual.equals<i64>({15, 27}));
}

TEST_CASE("reduce updates its tree when TSD membership changes and uses the default only when empty",
          "[feature:reduce][feature:nested-graph][series:tsd][validity:optional]") {
    const auto tested = [](wp<TS<i64>> left, wp<TS<i64>> right, wp<TS<bool>> include_left,
                           wp<TS<bool>> include_right) {
        return reduce(add_graph, dictionary_from_two_values(left, right, include_left, include_right),
                      "default"_n = std::int64_t{20});
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2, 3, 4}, std::array{4, 5, 6, 7},
                                            std::array{true, true, true, false},
                                            std::array{true, false, true, false});

    REQUIRE(actual.size() == 4);
    CHECK(actual[0].has_value());
    CHECK(actual[1].has_value());
    CHECK(actual[2].has_value());
    CHECK(actual[3].has_value());
    CHECK(scalar_cast<i64>(*actual[0]).value() == 5);
    CHECK(scalar_cast<i64>(*actual[1]).value() == 2);
    CHECK(scalar_cast<i64>(*actual[2]).value() == 9);
    CHECK(scalar_cast<i64>(*actual[3]).value() == 20);
}

TEST_CASE("reduce omits invalid items while valid items remain",
          "[feature:reduce][series:tsd][validity:optional][transition:invalidation]") {
    const auto tested = [](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> state) {
        return reduce(add_graph, dictionary_from_validity_state(first, second, state),
                      "default"_n = std::int64_t{20});
    };
    const auto actual = testing::test_graph(tested, std::array{5}, std::array{9}, std::array{0});

    CHECK(actual.equals<i64>({5}));
}

TEST_CASE("reduce uses the default when a valid TSD has keys but no valid items",
          "[feature:reduce][series:tsd][validity:optional][transition:invalidation]") {
    const auto tested = [](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> state) {
        return reduce(add_graph, dictionary_from_validity_state(first, second, state),
                      "default"_n = std::int64_t{20});
    };
    const auto actual = testing::test_graph(tested, std::array{5}, std::array{9}, std::array{1});

    CHECK(actual.equals<i64>({20}));
}

TEST_CASE("reduce uses the default when a valid TSD is empty",
          "[feature:reduce][series:tsd][validity:optional][transition:removal]") {
    const auto actual = testing::test_graph(namespace_scope_reduce_with_default, std::array{5}, std::array{9},
                                            std::array{2});

    CHECK(actual.equals<i64>({20}));
}

TEST_CASE("reduce does not use the default when the TSD is invalid",
          "[feature:reduce][series:tsd][validity:optional][transition:invalidation]") {
    const auto tested = [](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> state) {
        return reduce(add_graph, dictionary_from_validity_state(first, second, state),
                      "default"_n = std::int64_t{20});
    };
    const auto actual = testing::test_graph(tested, std::array{5}, std::array{9},
                                            std::array<std::optional<int>, 1>{std::nullopt});

    CHECK(actual.equals<i64>({std::nullopt}));
}

} // namespace
