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

TEST_CASE("reduce combines final TSD items with an ordinary compact reducer",
          "[feature:reduce][series:tsd][shape:single-node][guarantee:final]") {
    const auto final_dictionary_from_values = node(
        [](In<TS<i64>> left, In<TS<i64>> right,
           Out<TSD<std::int64_t, TS<i64, TimeSeriesGuarantee::final>>> output) {
            output[1].set(*left);
            output[2].set(*right);
        });
    const auto tested = [=](wp<TS<i64>> left, wp<TS<i64>> right) {
        return reduce(add_graph, final_dictionary_from_values(left, right));
    };

    const auto actual = testing::test_graph(tested, std::array{3}, std::array{4});

    CHECK(actual.equals<i64>({7}));
}

TEST_CASE("reduce combines final TSD items through a context-dependent fallback reducer",
          "[feature:reduce][feature:context-time-series][feature:nested-graph][series:tsd][shape:nested]"
          "[guarantee:final]") {
    const auto final_dictionary_from_values = node(
        [](In<TS<i64>> left, In<TS<i64>> right,
           Out<TSD<std::int64_t, TS<i64, TimeSeriesGuarantee::final>>> output) {
            output[1].set(*left);
            output[2].set(*right);
        });
    const auto tested = [=](wp<TS<i64>> left, wp<TS<i64>> right, wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        return reduce(context_add_graph, final_dictionary_from_values(left, right));
    };

    const auto actual = testing::test_graph(tested, std::array{3}, std::array{4}, std::array{10});

    CHECK(actual.equals<i64>({17}));
}

TEST_CASE("reduce retains untouched multi-node results and passive input state",
          "[feature:reduce][series:tsd][shape:chain][activity:passive][validity:optional][transition:update]") {
    const auto dictionary_from_four_values_with_one_update =
        node([](In<TS<i64>> first, In<TS<i64>> second, In<TS<i64>> third, In<TS<i64>> fourth,
                In<TS<bool>> initialize, In<TS<bool>> update_first, Out<TSD<std::int64_t, TS<i64>>> output) {
            if (*initialize) {
                output[1].set(*first);
                output[2].set(*second);
                output[3].set(*third);
                output[4].set(*fourth);
            } else if (*update_first) {
                output[1].set(*first);
            }
        });
    // Add a sentinel when the passive input is modified so stale modification state is observable.
    const auto sum_active_left_and_passive_right = node(
        [](In<TS<i64>> left, In<TS<i64>, passive, optional> right, Out<TS<i64>> output) {
            if (left.modified()) {
                output.set(*left + (right.valid() ? *right : i64{}) + (right.modified() ? i64{100} : i64{}));
            }
        });
    const auto copy_reduce_intermediate =
        node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto multi_node_passive_reducer = graph(
        "name"_n = "multi_node_passive_reducer", [=](wp<TS<i64>> left, wp<TS<i64>> right) {
            return copy_reduce_intermediate(sum_active_left_and_passive_right(left, opt(right)));
        });

    const auto tested = [=](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> third, wp<TS<i64>> fourth,
                            wp<TS<bool>> initialize, wp<TS<bool>> update_first) {
        return reduce(multi_node_passive_reducer,
                      dictionary_from_four_values_with_one_update(first, second, third, fourth, initialize,
                                                                  update_first));
    };

    const auto actual = testing::test_graph(tested, std::array{1, 10}, std::array{2, 2}, std::array{3, 3},
                                            std::array{4, 4}, std::array{true, false},
                                            std::array{false, true});

    CHECK(actual.equals<i64>({310, 119}));
}

TEST_CASE("reduce follows a reducer graph that returns its right input",
          "[feature:reduce][series:tsd][shape:nested][transition:update]") {
    const auto select_right = graph("name"_n = "select_reduce_right_input", [](wp<TS<i64>>, wp<TS<i64>> right) {
        return right;
    });
    const auto tested = [select_right](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> third) {
        return reduce(select_right, dictionary_from_three_values(first, second, third));
    };

    const auto actual = testing::test_graph(tested, std::array{1, 1}, std::array{4, 4}, std::array{7, 8});

    CHECK(actual.equals<i64>({7, 8}));
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

TEST_CASE("reduce follows a stable reference to a single TSD item",
          "[feature:reduce][series:tsd][series:reference][transition:update][transition:invalidation]"
          "[transition:revalidation]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TS<i64>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto retain_reference = node([](In<TS<bool>> install, In<REF<TS<i64>>, passive> target,
                                          Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
        if (*install) {
            output[1].copy(target);
        }
    });
    const auto tested = graph(
        "name"_n = "reduce_tracks_single_referenced_item",
        [&](wp<TS<i64>> command, wp<TS<bool>> install) -> wp<TS<i64>> {
            const auto target = make_target(command);
            return wp<TS<i64>>{reduce(add_graph, retain_reference(install, target))};
        });

    const auto actual = testing::test_graph(tested, std::array{5, 0, 9}, std::array{true, false, false});

    CHECK(actual.equals<i64>({5, std::nullopt, 9}));
}

TEST_CASE("reduce follows stable TSD references as initially invalid items become valid",
          "[feature:reduce][series:tsd][series:reference][transition:update][transition:invalidation]"
          "[transition:revalidation][shape:nested]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TS<i64>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto retain_references = node([](In<TS<bool>> install, In<REF<TS<i64>>, passive> first,
                                           In<REF<TS<i64>>, passive> second,
                                           Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
        if (*install) {
            output[1].copy(first);
            output[2].copy(second);
        }
    });
    const auto tested = graph(
        "name"_n = "reduce_tracks_referenced_items",
        [&](wp<TS<i64>> first_command, wp<TS<i64>> second_command, wp<TS<bool>> install) -> wp<TS<i64>> {
            const auto first = make_target(first_command);
            const auto second = make_target(second_command);
            return wp<TS<i64>>{reduce(add_graph, retain_references(install, first, second))};
        });

    const auto actual = testing::test_graph(tested, std::array{5, 6, 7, 8}, std::array{0, 9, 0, 11},
                                            std::array{true, false, false, false});

    CHECK(actual.equals<i64>({5, 15, 7, 19}));
}

} // namespace
