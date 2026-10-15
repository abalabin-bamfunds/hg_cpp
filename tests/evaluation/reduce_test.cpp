#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/reduce.hpp>
#include <bg/reduce/details/reduce_graph_argument.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace {

using namespace bg;
using namespace bg::literals;

const auto add_values = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) {
    output.set(*left + *right);
});

const auto multiply_values = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) {
    output.set(*left * *right);
});

const auto add_graph = graph("name"_n = "add_values_for_reduce", [](wp<TS<i64>> left, wp<TS<i64>> right) {
    return add_values(left, right);
});

const auto representation_sensitive_graph = graph(
    "name"_n = "representation_sensitive_reduce",
    [](wp<TS<i64>> left, wp<TS<i64>> right) {
        if (type_cast<TimeSeriesReferenceType>(&left.type()) != nullptr) {
            return add_values(left, right);
        }
        return multiply_values(left, right);
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
    "name"_n = "namespace_scope_reduce_with_default",
    [](wp<TSD<std::int64_t, TS<i64>>> dictionary) -> wp<TS<i64>> {
        return wp<TS<i64>>{reduce(add_graph, dictionary, "default"_n = std::int64_t{20})};
    });

TEST_CASE("reduce combines all valid TSD items through nested reducer graphs",
          "[feature:reduce][feature:nested-graph][series:tsd][shape:nested]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> values) {
        return reduce(add_graph, values);
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}, {2, 4}}, {{1, 2}, {2, 5}}, {{1, 3}, {2, 6}}});

    CHECK(actual.equals<i64>({5, 7, 9}));
}

TEST_CASE("reduce carries an odd item to the next level of its tree",
          "[feature:reduce][feature:nested-graph][series:tsd][shape:nested]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> values) {
        return reduce(add_graph, values);
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}, {2, 4}, {3, 7}}, {{1, 2}, {2, 5}, {3, 8}}});

    CHECK(actual.equals<i64>({12, 15}));
}

TEST_CASE("reduce combines final TSD items with a nested reducer",
          "[feature:reduce][feature:nested-graph][series:tsd][shape:nested][guarantee:final]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64, TimeSeriesGuarantee::final>>> values) {
        return reduce(add_graph, values);
    };

    const auto actual = testing::test_graph(tested, {{{1, 3}, {2, 4}}});

    CHECK(actual.equals<i64>({7}));
}

TEST_CASE("compact reduce does not propagate a dirty path when a child reducer does not publish",
          "[feature:reduce][series:tsd][shape:single-node][transition:update][notification:suppressed]") {
    const auto suppress_selected_value = graph(
        "name"_n = "suppress_selected_reduce_value", [](wp<TS<i64>> left, wp<TS<i64>> right) -> wp<TS<i64>> {
            const auto selected = node([](In<TS<i64>> left_value, In<TS<i64>> right_value, Out<TS<i64>> output) {
                if (*left_value != 99) {
                    output.set(*left_value + *right_value);
                }
            })(left, right);
            return wp<TS<i64>>{*selected};
        });
    const auto tested = [=](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(suppress_selected_value, dictionary);
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}, {2, 2}, {3, 3}, {4, 4}}, {{1, 99}}});

    CHECK(actual.equals<i64>({10, std::nullopt}));
}

TEST_CASE("compact reduce does not republish an unchanged root when membership changes",
          "[feature:reduce][series:tsd][shape:single-node][transition:removal][notification:suppressed]") {
    const auto suppress_item = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) {
        if (*left != 99) {
            output.set(*left + *right);
        }
    });
    const auto suppress_item_with_context = node(
        [](In<TS<i64>> left, In<TS<i64>> right, In<TS<i64>, "factor"_n, context> factor,
           Out<TS<i64>> output) {
            if (*left != 99) {
                output.set(*left + *right + *factor);
            }
        });
    const auto suppress_selected_value = graph("name"_n = "suppress_selected_reduce_value",
                                               [=](wp<TS<i64>> left, wp<TS<i64>> right) {
                                                   return suppress_item(left, right);
                                               });
    const auto suppress_selected_value_with_context = graph(
        "name"_n = "suppress_selected_reduce_value_with_context",
        [=](wp<TS<i64>> left, wp<TS<i64>> right) { return suppress_item_with_context(left, right); });
    const auto compact_graph = [=](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(suppress_selected_value, dictionary);
    };
    const auto fallback_graph = [=](wp<TSD<std::int64_t, TS<i64>>> dictionary, wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        return reduce(suppress_selected_value_with_context, dictionary);
    };
    const auto compact_output = testing::test_graph(
        compact_graph, {{{10, 1}, {20, 2}, {30, 3}, {40, 4}}, {{20, 99}, {10, literal::REMOVE}}});
    const auto fallback_output = testing::test_graph(
        fallback_graph, {{{10, 1}, {20, 2}, {30, 3}, {40, 4}}, {{20, 99}, {10, literal::REMOVE}}},
        std::array{0, 0});

    CHECK(compact_output.equals<i64>({10, std::nullopt}));
    CHECK(fallback_output.equals<i64>({10, std::nullopt}));
}

TEST_CASE("nested reduce preserves the reducer selected for reference-backed inputs",
          "[feature:reduce][series:tsd][shape:nested][binding:reference-to-value]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(representation_sensitive_graph, dictionary);
    };

    const auto actual = testing::test_graph(tested, {{{1, 3}, {2, 4}}});

    CHECK(actual.equals<i64>({7}));
}

TEST_CASE("nested reduce tracks keys when a sampled TSD reassigns physical item IDs",
          "[feature:reduce][series:tsd][series:reference][feature:item-id-remapping][transition:rebind]"
          "[shape:nested]") {
    const auto select_dictionary = node(
        [](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
           In<REF<TSD<std::int64_t, TS<i64>>>> second, Out<REF<TSD<std::int64_t, TS<i64>>>> output) {
            output.copy(*use_second ? second : first);
        });
    const auto tested = [=](wp<TS<bool>> use_second, wp<TSD<std::int64_t, TS<i64>>> first,
                            wp<TSD<std::int64_t, TS<i64>>> second) {
        const auto selected = select_dictionary(use_second, first.as<REF<TSD<std::int64_t, TS<i64>>>>(),
                                                second.as<REF<TSD<std::int64_t, TS<i64>>>>());
        return reduce(add_graph, selected);
    };

    const auto actual = testing::test_graph(tested, std::array{false, true, false, true},
                                            {{{10, 2}, {20, 3}, {30, 5}}},
                                            {{{30, 6}, {20, 4}, {10, 3}}});

    CHECK(actual.equals<i64>({10, 13, 10, 13}));
}

TEST_CASE("compact reduce handles addition that reuses an expired physical item ID",
          "[feature:reduce][series:tsd][transition:removal][transition:addition][storage:item-id-reuse]") {
    const auto make_dictionary = node([](In<TS<i64>> phase, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*phase == 0) {
            output[10].set(2);
            output[20].set(3);
            return;
        }

        if (*phase == 1) {
            output.erase(10);
            output[30].set(7);
            return;
        }

        output.erase(30);
        const auto new_item = output[40];
        const auto reused_id = output.item_id(40) == 0;
        new_item.set(reused_id ? i64{11} : i64{1'011});
    });
    const auto tested = [=](wp<TS<i64>> phase) { return reduce(add_graph, make_dictionary(phase)); };

    const auto actual = testing::test_graph(tested, std::array{0, 1, 2});

    REQUIRE(actual.size() == 3);
    CHECK((actual[0] && scalar_cast<i64>(*actual[0]).value() == 5));
    CHECK((actual[1] && scalar_cast<i64>(*actual[1]).value() == 10));
    CHECK((actual[2] && scalar_cast<i64>(*actual[2]).value() == 14));
}

TEST_CASE("compact reduce updates a sparse path in a larger TSD",
          "[feature:reduce][series:tsd][shape:single-node][transition:update][activity:sparse]") {
    const auto make_dictionary = node([](In<TS<bool>> initialize, In<TS<i64>> changed_key,
                                         In<TS<i64>> changed_value, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*initialize) {
            for (auto key = std::int64_t{1}; key <= 64; ++key) {
                output[key].set(key);
            }
            return;
        }

        output[*changed_key].set(*changed_value);
    });
    const auto tested = [=](wp<TS<bool>> initialize, wp<TS<i64>> changed_key, wp<TS<i64>> changed_value) {
        return reduce(add_graph, make_dictionary(initialize, changed_key, changed_value));
    };

    const auto actual = testing::test_graph(tested, std::array{true, false}, std::array{32, 32},
                                            std::array{100, 100});

    CHECK(actual.equals<i64>({2080, 2148}));
}

TEST_CASE("compact reduce timestamps a replacement input in its cycle and leaves it unchanged afterward",
          "[feature:reduce][series:tsd][shape:single-node][transition:removal][transition:update]") {
    const auto cycle_two = MIN_DT + EvaluationTime::duration{2};
    const auto cycle_three = MIN_DT + EvaluationTime::duration{3};
    auto retained_cycle_two_timestamp = false;
    const auto observe_inputs = node([&](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) {
        retained_cycle_two_timestamp = retained_cycle_two_timestamp ||
                                       (left.last_updated() == cycle_two && right.last_updated() == cycle_three);
        output.set(*left + *right);
    });
    const auto timestamp_reducer = graph("name"_n = "reduce_input_timestamp", [=](wp<TS<i64>> left,
                                                                                     wp<TS<i64>> right) {
        return observe_inputs(left, right);
    });
    const auto tested = [=](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(timestamp_reducer, dictionary);
    };

    testing::test_wiring([&] {
        const auto candidate = details::build_reduce_graph_argument_wiring(
            timestamp_reducer, TS<i64>::type(), TypeMap{});
        CHECK(candidate.graph.compact_graph.has_value());
    });

    const auto actual = testing::test_graph(
        tested, {{{1, 1}, {2, 2}, {3, 3}}, {{1, literal::REMOVE}}, {{3, 30}}});

    CHECK(actual.equals<i64>({6, 5, 32}));
    CHECK(retained_cycle_two_timestamp);
}

TEST_CASE("reduce combines final TSD items through a context-dependent fallback reducer",
          "[feature:reduce][feature:context-time-series][feature:nested-graph][series:tsd][shape:nested]"
          "[guarantee:final]") {
    const auto tested = [=](wp<TSD<std::int64_t, TS<i64, TimeSeriesGuarantee::final>>> dictionary,
                            wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        return reduce(context_add_graph, dictionary);
    };

    const auto actual = testing::test_graph(tested, {{{1, 3}, {2, 4}}}, std::array{10});

    CHECK(actual.equals<i64>({17}));
}

TEST_CASE("reduce retains untouched multi-node results and passive input state",
          "[feature:reduce][series:tsd][shape:chain][activity:passive][validity:optional][transition:update]") {
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

    const auto tested = [=](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(multi_node_passive_reducer, dictionary);
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}, {2, 2}, {3, 3}, {4, 4}}, {{1, 10}}});

    CHECK(actual.equals<i64>({310, 119}));
}

TEST_CASE("reduce adapts a generic lambda to the TSD item type",
          "[feature:reduce][feature:operator][series:tsd][definition:function]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> values) {
        return reduce([](auto first, auto second) { return add_values(first, second); }, values);
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}, {2, 4}}, {{1, 2}, {2, 5}}});

    CHECK(actual.equals<i64>({5, 7}));
}

TEST_CASE("reduce carries inherited context references into its reducer graphs",
          "[feature:reduce][feature:context-time-series][feature:nested-graph]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> dictionary, wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        return reduce(context_add_graph, dictionary);
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}, {2, 4}}, {{1, 2}, {2, 5}}},
                                            std::array{10, 20});

    CHECK(actual.equals<i64>({15, 27}));
}

TEST_CASE("reduce fallback reads values from reference TSD items with inherited context",
          "[feature:reduce][feature:context-time-series][feature:nested-graph][series:tsd]"
          "[series:reference][binding:reference-to-value][transition:update]") {
    const auto make_target = node([](In<TS<i64>> value, Out<TS<i64>> output) { output.set(*value); });
    const auto retain_references = node([](In<TS<bool>> install, In<REF<TS<i64>>, passive> first,
                                           In<REF<TS<i64>>, passive> second,
                                           Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
        if (*install) {
            output[1].copy(first);
            output[2].copy(second);
        }
    });
    const auto tested = [=](wp<TS<i64>> first_value, wp<TS<i64>> second_value, wp<TS<bool>> install,
                            wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        const auto first = make_target(first_value);
        const auto second = make_target(second_value);
        return reduce(context_add_graph, retain_references(install, first, second));
    };

    const auto actual = testing::test_graph(tested, std::array{3, 4, 5}, std::array{8, 9, 10},
                                            std::array{true, false, false}, std::array{2, 2, 2});

    CHECK(actual.equals<i64>({13, 15, 17}));
}

TEST_CASE("reduce publishes a singleton fallback value after retiring a pair graph",
          "[feature:reduce][feature:nested-graph][series:tsd][shape:nested]"
          "[transition:removal][transition:update]") {
    const auto tested = [=](wp<TSD<std::int64_t, TS<i64>>> dictionary, wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        return reduce(context_add_graph, dictionary);
    };

    const auto actual = testing::test_graph(tested,
                                            {{{1, 3}, {2, 5}}, {{1, 4}, {2, literal::REMOVE}}, {{1, 7}}},
                                            std::array{2, 2, 2});

    CHECK(actual.equals<i64>({10, 4, 7}));
}

TEST_CASE("reduce fallback refreshes a singleton reference item after selection cleanup",
          "[feature:reduce][feature:context-time-series][feature:nested-graph][series:tsd]"
          "[series:reference][binding:reference-to-value][transition:update][transition:invalidation]"
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
    const auto tested = [=](wp<TS<i64>> command, wp<TS<bool>> install, wp<TS<i64>> factor) {
        auto context_scope = with_context("factor"_n = factor);
        const auto target = make_target(command);
        return reduce(context_add_graph, retain_reference(install, target));
    };

    const auto actual = testing::test_graph(tested, std::array{5, 0, 9}, std::array{true, false, false},
                                            std::array{2, 2, 2});

    CHECK(actual.equals<i64>({5, std::nullopt, 9}));
}

TEST_CASE("reduce updates its tree when TSD membership changes and uses the default only when empty",
          "[feature:reduce][feature:nested-graph][series:tsd][validity:optional]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(add_graph, dictionary, "default"_n = std::int64_t{20});
    };

    const auto actual = testing::test_graph(
        tested, {{{1, 1}, {2, 4}}, {{1, 2}, {2, literal::REMOVE}}, {{1, 3}, {2, 6}},
                 {{1, literal::REMOVE}, {2, literal::REMOVE}}});

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
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(add_graph, dictionary, "default"_n = std::int64_t{20});
    };
    const auto actual = testing::test_graph(tested, {{{1, 5}, {2, literal::INVALID}}});

    CHECK(actual.equals<i64>({5}));
}

TEST_CASE("reduce uses the default when a valid TSD has keys but no valid items",
          "[feature:reduce][series:tsd][validity:optional][transition:invalidation]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(add_graph, dictionary, "default"_n = std::int64_t{20});
    };
    const auto actual = testing::test_graph(tested, {{{1, literal::INVALID}, {2, literal::INVALID}}});

    CHECK(actual.equals<i64>({20}));
}

TEST_CASE("reduce uses the default when a valid TSD is empty",
          "[feature:reduce][series:tsd][validity:optional][transition:removal]") {
    const auto actual = testing::test_graph(namespace_scope_reduce_with_default, {{}});

    CHECK(actual.equals<i64>({20}));
}

TEST_CASE("reduce does not use the default when the TSD is invalid",
          "[feature:reduce][series:tsd][validity:optional][transition:invalidation]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<i64>>> dictionary) {
        return reduce(add_graph, dictionary, "default"_n = std::int64_t{20});
    };
    const auto actual = testing::test_graph(tested, {literal::INVALID});

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

    REQUIRE(actual.size() == 4);
    CHECK((actual[0] && scalar_cast<i64>(*actual[0]).value() == 5));
    CHECK((actual[1] && scalar_cast<i64>(*actual[1]).value() == 15));
    CHECK((actual[2] && scalar_cast<i64>(*actual[2]).value() == 7));
    CHECK((actual[3] && scalar_cast<i64>(*actual[3]).value() == 19));
}

} // namespace
