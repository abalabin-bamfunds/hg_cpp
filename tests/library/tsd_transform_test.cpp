#include <bg/library.hpp>
#include <bg/evaluation_profile.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/run_graph.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace bg;

TEST_CASE("rekey routes source time series and applies mapping changes before assignments",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change][validity:optional]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto rekey = [&](wp<TSD<i64, TS<i64>>> source, wp<TSD<i64, TS<string>>> new_keys) {
        return materialize(operators::rekey(source, new_keys));
    };

    CHECK(bg::run_graph(rekey,
                        bg::input<TSD<i64, TS<i64>>>({{{1, 10}, {2, 20}}, {{1, 11}},
                                                      {{1, literal::REMOVE}}, {{1, 12}}}),
                        bg::input<TSD<i64, TS<string>>>({{{1, "a"}, {2, "b"}}, {{1, "c"}}, {}, {}}))
              .equals({{{"a", 10}, {"b", 20}}, {{"a", literal::REMOVE}, {"c", 11}},
                       {{"c", literal::REMOVE}}, {{"c", 12}}}));
}

TEST_CASE("rekey processes removals before colliding updates and does not restore an overwritten source",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto rekey = [&](wp<TSD<i64, TS<i64>>> source, wp<TSD<i64, TS<string>>> new_keys) {
        return materialize(operators::rekey(source, new_keys));
    };

    CHECK(bg::run_graph(rekey, bg::input<TSD<i64, TS<i64>>>({{{1, 10}, {2, 20}}, {{2, literal::REMOVE}}}),
                        bg::input<TSD<i64, TS<string>>>({{{1, "shared"}, {2, "shared"}}, {}}))
              .equals({{{"shared", 20}}, {{"shared", literal::REMOVE}}}));
}

TEST_CASE("rekey applies a mapping-only change without requiring a source tick",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> keys) {
        return materialize(operators::rekey(values, keys));
    };

    CHECK(bg::run_graph(graph, {{{1, 10}}, {{1, 11}}, literal::SKIP},
                        {{{1, "first"}}, literal::SKIP, {{1, "second"}}})
              .equals({{{"first", 10}}, {{"first", 11}}, {{"first", literal::REMOVE}, {"second", 11}}}));
}

TEST_CASE("partition applies a mapping-only change without requiring a source tick",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<string, TSD<i64, TS<i64>>>> input,
                                     Out<TSD<string, TSD<i64, TS<i64>>>> output) { output.copy_tick(input); });
    const auto graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> partitions) {
        return materialize(operators::partition(values, partitions));
    };

    CHECK(bg::run_graph(graph, {{{1, 10}}, {{1, 11}}, literal::SKIP},
                        {{{1, "odd"}}, literal::SKIP, {{1, "even"}}})
              .equals({{{"odd", {{1, 10}}}}, {{"odd", {{1, 11}}}},
                       {{"odd", literal::REMOVE}, {"even", {{1, 11}}}}}));
}

TEST_CASE("rekey publishes a valid empty collection and removes routes when a source item is removed",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[validity:optional][transition:removal]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto rekey = [&](wp<TSD<i64, TS<i64>>> source, wp<TSD<i64, TS<string>>> new_keys) {
        return materialize(operators::rekey(source, new_keys));
    };

    CHECK(bg::run_graph(rekey,
                        bg::input<TSD<i64, TS<i64>>>({{}, {{1, 10}}, {{1, literal::REMOVE}}}),
                        bg::input<TSD<i64, TS<string>>>({{}, {{1, "a"}}, literal::SKIP}))
              .equals({{}, {{"a", 10}}, {{"a", literal::REMOVE}}}));
}

TEST_CASE("rekey removes routes when the source or mapping reference is rebound to an invalid dictionary",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[transition:update][transition:invalidation][transition:revalidation][binding:reference-to-value]") {
    const auto select_source = node(
        [](In<TS<bool>> select_invalid, In<REF<TSD<i64, TS<i64>>>> valid,
           In<REF<TSD<i64, TS<i64>>>> invalid, Out<REF<TSD<i64, TS<i64>>>> output) {
            output.copy(*select_invalid ? invalid : valid);
        });
    const auto select_mapping = node(
        [](In<TS<bool>> select_invalid, In<REF<TSD<i64, TS<string>>>> valid,
           In<REF<TSD<i64, TS<string>>>> invalid, Out<REF<TSD<i64, TS<string>>>> output) {
            output.copy(*select_invalid ? invalid : valid);
        });
    const auto invalid_source = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto invalid_mapping = node([](Out<TSD<i64, TS<string>>>) {});
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TS<bool>> select_invalid_source, wp<TS<bool>> select_invalid_mapping,
                           wp<TSD<i64, TS<i64>>> source, wp<TSD<i64, TS<string>>> mapping) {
        return materialize(operators::rekey(select_source(select_invalid_source, source, invalid_source()),
                                            select_mapping(select_invalid_mapping, mapping, invalid_mapping())));
    };

    CHECK(bg::run_graph(
              graph, bg::input<TS<bool>>({false, true, false, false}),
              bg::input<TS<bool>>({false, false, false, true}),
              bg::input<TSD<i64, TS<i64>>>({{{1, 10}}, literal::SKIP, literal::SKIP, literal::SKIP}),
              bg::input<TSD<i64, TS<string>>>({{{1, "a"}}, literal::SKIP, literal::SKIP, literal::SKIP}))
              .equals({{{"a", 10}}, {{"a", literal::REMOVE}}, {{"a", 10}}, {{"a", literal::REMOVE}}}));
}

namespace {

struct nested_dictionary_child_validity_observation {
    bool outer_valid;
    bool has_outer_key;
    bool child_valid;
    bool became_invalid;
    bool became_valid;
    bool has_inner_key;
    i64 value;
    i64 last_updated;
    i64 probe;
};

} // namespace

TEST_CASE("nested dictionary reference retains invalid child state while its target is unchanged",
          "[feature:reference][series:tsd][shape:nested][transition:invalidation][transition:revalidation]") {
    auto observations = std::vector<nested_dictionary_child_validity_observation>{};
    const auto valid_child = node([](In<TS<i64>> tick, Out<TSD<i64, TS<i64>>> output) {
        if (*tick == 0) {
            output[10].set(10);
        } else if (*tick == 3) {
            output[10].set(20);
        }
    });
    const auto invalid_child = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto select_child = node(
        [](In<TS<i64>> tick, In<REF<TSD<i64, TS<i64>>>> valid_child,
           In<REF<TSD<i64, TS<i64>>>> invalid_child,
           Out<TSD<i64, REF<TSD<i64, REF<TS<i64>>>>>> output) {
            if (*tick == 0 || *tick == 3) {
                output[1].copy(valid_child);
            } else if (*tick == 1) {
                output[1].copy(invalid_child);
            }
        });
    const auto preserve_references = node([](In<REF<TSD<i64, TSD<i64, REF<TS<i64>>>>>> input,
                                             Out<REF<TSD<i64, TSD<i64, REF<TS<i64>>>>>> output) {
        output.copy(input);
    });
    const auto observe = node([&](In<TSD<i64, TSD<i64, TS<i64>>>, optional> input, In<TS<i64>> probe_value) {
        const auto has_outer_key = input.valid() && input.contains(1);
        const auto child = has_outer_key ? input[1] : In<TSD<i64, TS<i64>>>{};
        const auto has_inner_key = child.valid() && child.contains(10);
        const auto value = has_inner_key ? *child[10] : i64{-1};
        const auto last_updated = static_cast<i64>(child.last_updated().time_since_epoch().count());
        observations.push_back({input.valid(), has_outer_key, child.valid(), child.became_invalid(),
                                child.became_valid(), has_inner_key, value, last_updated, *probe_value});
    });
    const auto graph = [&](wp<TS<i64>> tick) {
        const auto selected = select_child(tick, valid_child(tick), invalid_child());
        observe(preserve_references(selected), tick);
    };

    (void)bg::run_graph(graph, {0, 1, 2, 3});

    REQUIRE(observations.size() == 4);
    CHECK(observations[0].outer_valid);
    CHECK(observations[0].has_outer_key);
    CHECK(observations[0].child_valid);
    CHECK(observations[0].has_inner_key);
    CHECK(observations[0].value == 10);
    CHECK(observations[1].outer_valid);
    CHECK(observations[1].has_outer_key);
    CHECK_FALSE(observations[1].child_valid);
    CHECK(observations[1].became_invalid);
    CHECK_FALSE(observations[1].has_inner_key);
    CHECK(observations[2].outer_valid);
    CHECK(observations[2].has_outer_key);
    CHECK_FALSE(observations[2].child_valid);
    CHECK_FALSE(observations[2].became_invalid);
    CHECK_FALSE(observations[2].became_valid);
    CHECK(observations[2].last_updated == observations[1].last_updated);
    CHECK(observations[2].probe == 2);
    CHECK(observations[3].outer_valid);
    CHECK(observations[3].has_outer_key);
    CHECK(observations[3].child_valid);
    CHECK(observations[3].became_valid);
    CHECK(observations[3].has_inner_key);
    CHECK(observations[3].value == 20);
    CHECK(observations[1].last_updated < observations[3].last_updated);
}

TEST_CASE("partition moves a source key by removing its old group before assignment",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize_partition =
        node([](In<TSD<string, TSD<i64, TS<i64>>>> input,
                Out<TSD<string, TSD<i64, TS<i64>>>> output) { output.copy_tick(input); });
    const auto partition_graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> partitions) {
        return materialize_partition(operators::partition(values, partitions));
    };

    CHECK(bg::run_graph(partition_graph,
                        bg::input<TSD<i64, TS<i64>>>({{{1, 10}, {2, 20}}, {{1, 11}}}),
                        bg::input<TSD<i64, TS<string>>>({{{1, "odd"}, {2, "even"}}, {{1, "even"}}}))
              .equals({{{"odd", {{1, 10}}}, {"even", {{2, 20}}}},
                       {{"odd", literal::REMOVE}, {"even", {{1, 11}}}}}));
}

TEST_CASE("partition keeps empty groups when remove_empty is false",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<string, TSD<i64, TS<i64>>>> input,
                                     Out<TSD<string, TSD<i64, TS<i64>>>> output) { output.copy_tick(input); });
    const auto graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> partitions) {
        return materialize(operators::partition(values, partitions, false));
    };

    CHECK(bg::run_graph(graph, bg::input<TSD<i64, TS<i64>>>({{{1, 10}}}),
                        bg::input<TSD<i64, TS<string>>>({{{1, "odd"}}, {{1, "even"}}}))
              .equals({{{"odd", {{1, 10}}}}, {{"odd", {{1, literal::REMOVE}}}, {"even", {{1, 10}}}}}));
}

TEST_CASE("partition remove_empty is a scalar option",
          "[feature:operator][feature:type-system][series:tsd]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> partitions,
                          wp<TS<bool>> remove_empty) {
        return operators::partition(values, partitions, remove_empty);
    };

    CHECK_THROWS_AS(bg::run_graph(graph, {{{1, 10}}}, {{{1, "odd"}}}, {true}), WiringError);
}

struct partition_values_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSD<i64, TS<i64>>> output;
    State<i64> tick;

    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate(EvaluationContext *context) const {
        switch ((*tick)++) {
        case 0:
            output[1].set(10);
            output[2].set(20);
            break;
        case 1: output[1].set(11); break;
        case 2: output[2].set(21); break;
        default: return;
        }

        if (*tick < 3) {
            context->schedule_at(context->evaluation_time() + EvaluationTime::duration{1});
        }
    }
};

struct partition_labels_source {
    Out<TSD<i64, TS<string>>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const {
        output[1].set("odd");
        output[2].set("even");
    }
};

TEST_CASE("partition leaf updates reach consumers without evaluating the transform",
          "[feature:operator][feature:reference][feature:profiling][series:tsd][feature:collection-change]"
          "[shape:multiple][transition:first-tick][transition:update]") {
    auto observations = std::vector<i64>{};
    const auto observe = node([&](In<TSD<string, TSD<i64, TS<i64>>>> groups) {
        auto total = i64{};
        groups.for_each_value([&](const auto &group) {
            group.for_each_value([&](const auto &value) { total += *value; });
        });
        observations.push_back(total);
    });
    const auto values_source = node<partition_values_source>();
    const auto partitions_source = node<partition_labels_source>();

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)observe(operators::partition(values_source(), partitions_source()));
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    SimulationEvaluationEngine{}.run(runtime, MIN_DT + EvaluationTime::duration{1}, profile);
    profile.finish();

    CHECK(observations == std::vector<i64>{30, 31, 32});

    auto partition_evaluations = std::size_t{};
    auto observed_node_evaluations = std::size_t{};
    auto reader = EvaluationProfileReader{profile.take_recording()};
    while (const auto record = reader.read()) {
        const auto *node_record = std::get_if<EvaluationProfileNode>(&*record);
        if (node_record == nullptr) {
            continue;
        }

        const auto &wiring_path = compiled.wiring_path(node_record->node_index);
        if (wiring_path.name.text() == "partition") {
            ++partition_evaluations;
        }
        if (wiring_path.name.text().empty()) {
            ++observed_node_evaluations;
        }
    }

    CHECK(partition_evaluations == 1);
    CHECK(observed_node_evaluations == 3);
}

TEST_CASE("partition follows source value ticks between key moves",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[transition:update]") {
    const auto read_even = node([](In<TSD<string, TSD<i64, TS<i64>>>> groups) {
        return groups.contains("even") && groups["even"].contains(1) ? *groups["even"][1] : i64{-1};
    });
    const auto graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> partitions) {
        return read_even(operators::partition(values, partitions));
    };

    CHECK(bg::run_graph(graph, bg::input<TSD<i64, TS<i64>>>({{{1, 10}}, {{1, 11}}, {{1, 12}}}),
                        bg::input<TSD<i64, TS<string>>>({{{1, "odd"}}, literal::SKIP, {{1, "even"}}}))
              .equals<i64>({-1, -1, 12}));
}

TEST_CASE("partition moves keys across groups before assigning into groups emptied in the same tick",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[transition:update][shape:multiple]") {
    const auto materialize = node([](In<TSD<string, TSD<i64, TS<i64>>>> input,
                                     Out<TSD<string, TSD<i64, TS<i64>>>> output) { output.copy_tick(input); });
    const auto graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<string>>> partitions) {
        return materialize(operators::partition(values, partitions));
    };

    CHECK(bg::run_graph(graph, {{{1, 10}, {2, 20}}, literal::SKIP},
                        {{{1, "odd"}, {2, "even"}}, {{1, "even"}, {2, "odd"}}})
              .equals({{{"odd", {{1, 10}}}, {"even", {{2, 20}}}},
                       {{"odd", {{1, literal::REMOVE}, {2, 20}}},
                        {"even", {{2, literal::REMOVE}, {1, 10}}}}}));
}

TEST_CASE("flip_keys transposes nested dictionaries and removes empty outer groups",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize_flip_keys =
        node([](In<TSD<string, TSD<i64, TS<i64>>>> input,
                Out<TSD<string, TSD<i64, TS<i64>>>> output) { output.copy_tick(input); });
    const auto flip_keys_graph = [&](wp<TSD<i64, TSD<string, TS<i64>>>> input) {
        return materialize_flip_keys(operators::flip_keys(input));
    };

    CHECK(bg::run_graph(flip_keys_graph,
                        bg::input<TSD<i64, TSD<string, TS<i64>>>>({{{1, {{"a", 5}, {"b", 6}}},
                                                                     {2, {{"b", 7}}}},
                                                                    {{1, {{"a", literal::REMOVE}}}}}))
              .equals({{{"a", {{1, 5}}}, {"b", {{1, 6}, {2, 7}}}}, {{"a", literal::REMOVE}}}));
}

TEST_CASE("flip_keys keeps empty groups when remove_empty is false",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<string, TSD<i64, TS<i64>>>> input,
                                     Out<TSD<string, TSD<i64, TS<i64>>>> output) { output.copy_tick(input); });
    const auto graph = [&](wp<TSD<i64, TSD<string, TS<i64>>>> input) {
        return materialize(operators::flip_keys(input, false));
    };

    CHECK(bg::run_graph(graph, bg::input<TSD<i64, TSD<string, TS<i64>>>>({{{1, {{"a", 5}}}},
                                                                            {{1, {{"a", literal::REMOVE}}}}}))
              .equals({{{"a", {{1, 5}}}}, {{"a", {{1, literal::REMOVE}}}}}));
}

TEST_CASE("collapse_keys and uncollapse_keys preserve nested collection deltas",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize_collapsed = node([](In<TSD<Tuple<i64, string>, TS<i64>>> input,
                                               Out<TSD<Tuple<i64, string>, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto collapse_graph = [&](wp<TSD<i64, TSD<string, TS<i64>>>> input) {
        return materialize_collapsed(operators::collapse_keys(input));
    };

    CHECK(bg::run_graph(collapse_graph,
                        bg::input<TSD<i64, TSD<string, TS<i64>>>>({{{1, {{"a", 5}, {"b", 6}}}},
                                                                   {{1, {{"a", literal::REMOVE}}}}}))
              .equals({{{{1, "a"}, 5}, {{1, "b"}, 6}}, {{{1, "a"}, literal::REMOVE}}}));

    const auto materialize_uncollapsed =
        node([](In<TSD<i64, TSD<string, TS<i64>>>> input,
                Out<TSD<i64, TSD<string, TS<i64>>>> output) { output.copy_tick(input); });
    const auto uncollapse_graph = [&](wp<TSD<Tuple<i64, string>, TS<i64>>> input) {
        return materialize_uncollapsed(operators::uncollapse_keys(input));
    };

    CHECK(bg::run_graph(uncollapse_graph,
                        bg::input<TSD<Tuple<i64, string>, TS<i64>>>({{{{1, "a"}, 5}, {{1, "b"}, 6}},
                                                                     {{{1, "a"}, literal::REMOVE}}}))
              .equals({{{1, {{"a", 5}, {"b", 6}}}}, {{1, {{"a", literal::REMOVE}}}}}));
}

TEST_CASE("uncollapse_keys keeps empty groups when remove_empty is false",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<i64, TSD<string, TS<i64>>>> input,
                                     Out<TSD<i64, TSD<string, TS<i64>>>> output) { output.copy_tick(input); });
    const auto graph = [&](wp<TSD<Tuple<i64, string>, TS<i64>>> input) {
        return materialize(operators::uncollapse_keys(input, false));
    };

    CHECK(bg::run_graph(graph, bg::input<TSD<Tuple<i64, string>, TS<i64>>>({{{{1, "a"}, 5}},
                                                                              {{{1, "a"}, literal::REMOVE}}}))
              .equals({{{1, {{"a", 5}}}}, {{1, {{"a", literal::REMOVE}}}}}));
}

TEST_CASE("unpartition flattens nested dictionary changes",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]") {
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TSD<string, TSD<i64, TS<i64>>>> input) {
        return materialize(operators::unpartition(input));
    };

    CHECK(bg::run_graph(graph,
                        bg::input<TSD<string, TSD<i64, TS<i64>>>>({{{"odd", {{1, 10}}}, {"even", {{2, 20}}}},
                                                                    {{"odd", {{1, literal::REMOVE}}}}}))
              .equals({{{1, 10}, {2, 20}}, {{1, literal::REMOVE}}}));
}

TEST_CASE("unpartition applies removals before assignments across groups",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[transition:removal][transition:update]") {
    const auto read_key = node([](In<TSD<i64, TS<i64>>> input) {
        return input.contains(1) ? *input[1] : i64{-1};
    });
    const auto graph = [&](wp<TSD<string, TSD<i64, TS<i64>>>> input) {
        return read_key(operators::unpartition(input));
    };

    CHECK(bg::run_graph(graph,
                        bg::input<TSD<string, TSD<i64, TS<i64>>>>({{{"B", {{2, 20}}}, {"A", {{1, 10}}}},
                                                                   {{"B", {{1, 11}}}, {"A", {{1, literal::REMOVE}}}},
                                                                   {{"B", {{1, 12}}}}}))
              .equals<i64>({10, 11, 12}));
}

TEST_CASE("unpartition refreshes a group when a referenced dictionary changes item IDs",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[feature:item-id-remapping][binding:reference-to-value][transition:update]") {
    auto remappings = std::vector<bool>{};
    auto snapshots = std::vector<std::vector<std::pair<i64, i64>>>{};
    const auto select_source = node(
        [](In<TS<bool>> select_second, In<REF<TSD<string, TSD<i64, REF<TS<i64>>>>>> first,
           In<REF<TSD<string, TSD<i64, REF<TS<i64>>>>>> second,
           Out<REF<TSD<string, TSD<i64, REF<TS<i64>>>>>> output) {
            output.copy(*select_second ? second : first);
        });
    const auto inspect_remapping = node([&](In<TS<bool>> select_second,
                                            In<TSD<string, TSD<i64, TS<i64>>>> input) {
        if (!*select_second) {
            return;
        }

        remappings.push_back(input.has_item_id_remapping());
        auto snapshot = std::vector<std::pair<i64, i64>>{};
        input.for_each_value([&](const auto &group) {
            group.for_each_key_value([&](const auto &key, const auto &value) {
                snapshot.emplace_back(key.value(), *value);
            });
        });
        std::ranges::sort(snapshot);
        snapshots.push_back(std::move(snapshot));
    });
    const auto sum = node([](In<TSD<i64, TS<i64>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &value) { total += *value; });
        return total;
    });
    const auto graph = [&](wp<TS<bool>> select_second, wp<TSD<string, TSD<i64, TS<i64>>>> first,
                           wp<TSD<string, TSD<i64, TS<i64>>>> second) {
        const auto selected = select_source(select_second, first, second);
        inspect_remapping(select_second, selected);
        return sum(operators::unpartition(selected));
    };

    CHECK(bg::run_graph(graph, {false, true, literal::SKIP},
                        {{{"odd", {{1, 10}}}, {"even", {{2, 20}}}}, literal::SKIP, literal::SKIP},
                        {{{"even", {{2, 200}}}, {"odd", {{1, 100}}}}, literal::SKIP, {{"even", {{2, 201}}}}})
              .equals<i64>({30, 300, 301}));
    CHECK(remappings == std::vector{true, false});
    CHECK(snapshots == std::vector<std::vector<std::pair<i64, i64>>>{{{1, 100}, {2, 200}},
                                                                     {{1, 100}, {2, 201}}});
}

TEST_CASE("flip_keys and collapse_keys refresh nested items when referenced dictionaries exchange IDs",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[feature:item-id-remapping][binding:reference-to-value][transition:update]") {
    auto flipped_snapshots = std::vector<std::vector<std::tuple<std::string, i64, i64>>>{};
    auto collapsed_snapshots = std::vector<std::vector<std::tuple<i64, std::string, i64>>>{};
    const auto select_source = node(
        [](In<TS<bool>> select_second, In<REF<TSD<i64, TSD<string, REF<TS<i64>>>>>> first,
           In<REF<TSD<i64, TSD<string, REF<TS<i64>>>>>> second,
           Out<REF<TSD<i64, TSD<string, REF<TS<i64>>>>>> output) {
            output.copy(*select_second ? second : first);
        });
    const auto sum_flipped = node([](In<TSD<string, TSD<i64, TS<i64>>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &group) {
            group.for_each_value([&](const auto &value) { total += *value; });
        });
        return total;
    });
    const auto sum_collapsed = node([](In<TSD<Tuple<i64, string>, TS<i64>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &value) { total += *value; });
        return total;
    });
    const auto observe_flipped = node([&](In<TSD<string, TSD<i64, TS<i64>>>> input) {
        auto snapshot = std::vector<std::tuple<std::string, i64, i64>>{};
        input.for_each_key_value([&](const auto &group_key, const auto &group) {
            group.for_each_key_value([&](const auto &key, const auto &value) {
                snapshot.emplace_back(std::string{group_key.value()}, key.value(), *value);
            });
        });
        std::ranges::sort(snapshot);
        flipped_snapshots.push_back(std::move(snapshot));
    });
    const auto observe_collapsed = node([&](In<TSD<Tuple<i64, string>, TS<i64>>> input) {
        auto snapshot = std::vector<std::tuple<i64, std::string, i64>>{};
        input.for_each_key_value([&](const auto &key, const auto &value) {
            snapshot.emplace_back(key.template get<0>().value(),
                                  std::string{key.template get<1>().value()}, *value);
        });
        std::ranges::sort(snapshot);
        collapsed_snapshots.push_back(std::move(snapshot));
    });
    const auto flip_graph = [&](wp<TS<bool>> select_second, wp<TSD<i64, TSD<string, TS<i64>>>> first,
                                wp<TSD<i64, TSD<string, TS<i64>>>> second) {
        const auto flipped = operators::flip_keys(select_source(select_second, first, second));
        observe_flipped(flipped);
        return sum_flipped(flipped);
    };
    const auto collapse_graph = [&](wp<TS<bool>> select_second, wp<TSD<i64, TSD<string, TS<i64>>>> first,
                                    wp<TSD<i64, TSD<string, TS<i64>>>> second) {
        const auto collapsed = operators::collapse_keys(select_source(select_second, first, second));
        observe_collapsed(collapsed);
        return sum_collapsed(collapsed);
    };

    CHECK(bg::run_graph(flip_graph, {false, true, literal::SKIP},
                        {{{1, {{"a", 5}, {"b", 6}}}, {2, {{"b", 7}}}}, literal::SKIP, literal::SKIP},
                        {{{2, {{"b", 70}}}, {1, {{"b", 60}, {"a", 50}}}}, literal::SKIP,
                         {{1, {{"a", 51}}}}})
              .equals<i64>({18, 180, 181}));
    CHECK(bg::run_graph(collapse_graph, {false, true, literal::SKIP},
                        {{{1, {{"a", 5}, {"b", 6}}}, {2, {{"b", 7}}}}, literal::SKIP, literal::SKIP},
                        {{{2, {{"b", 70}}}, {1, {{"b", 60}, {"a", 50}}}}, literal::SKIP,
                         {{1, {{"a", 51}}}}})
              .equals<i64>({18, 180, 181}));
    CHECK(flipped_snapshots == std::vector<std::vector<std::tuple<std::string, i64, i64>>>{
                                   {{"a", 1, 5}, {"b", 1, 6}, {"b", 2, 7}},
                                   {{"a", 1, 50}, {"b", 1, 60}, {"b", 2, 70}},
                                   {{"a", 1, 51}, {"b", 1, 60}, {"b", 2, 70}}});
    CHECK(collapsed_snapshots == std::vector<std::vector<std::tuple<i64, std::string, i64>>>{
                                     {{1, "a", 5}, {1, "b", 6}, {2, "b", 7}},
                                     {{1, "a", 50}, {1, "b", 60}, {2, "b", 70}},
                                     {{1, "a", 51}, {1, "b", 60}, {2, "b", 70}}});
}

TEST_CASE("flip_keys and collapse_keys refresh an inner referenced dictionary after its item IDs exchange",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[feature:item-id-remapping][binding:reference-to-value][transition:update]") {
    auto remappings = std::vector<bool>{};
    auto flipped_snapshots = std::vector<std::vector<std::tuple<i64, std::string, i64>>>{};
    auto collapsed_snapshots = std::vector<std::vector<std::tuple<std::string, i64, i64>>>{};
    const auto select_group = node(
        [](In<TS<bool>> select_second, In<REF<TSD<i64, REF<TS<i64>>>>> first,
           In<REF<TSD<i64, REF<TS<i64>>>>> second, Out<REF<TSD<i64, REF<TS<i64>>>>> output) {
            output.copy(*select_second ? second : first);
        });
    const auto inspect_group = node([&](In<TS<bool>> select_second, In<TSD<i64, TS<i64>>> group) {
        if (!*select_second) {
            return;
        }

        remappings.push_back(group.has_item_id_remapping());
    });
    const auto make_outer = node([](In<REF<TSD<i64, REF<TS<i64>>>>> group,
                                    Out<TSD<string, REF<TSD<i64, REF<TS<i64>>>>>> output) {
        output["group"].copy(group);
    });
    const auto sum_flipped = node([](In<TSD<i64, TSD<string, TS<i64>>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &group) {
            group.for_each_value([&](const auto &value) { total += *value; });
        });
        return total;
    });
    const auto sum_collapsed = node([](In<TSD<Tuple<string, i64>, TS<i64>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &value) { total += *value; });
        return total;
    });
    const auto observe_flipped = node([&](In<TSD<i64, TSD<string, TS<i64>>>> input) {
        auto snapshot = std::vector<std::tuple<i64, std::string, i64>>{};
        input.for_each_key_value([&](const auto &key, const auto &group) {
            group.for_each_key_value([&](const auto &group_key, const auto &value) {
                snapshot.emplace_back(key.value(), std::string{group_key.value()}, *value);
            });
        });
        std::ranges::sort(snapshot);
        flipped_snapshots.push_back(std::move(snapshot));
    });
    const auto observe_collapsed = node([&](In<TSD<Tuple<string, i64>, TS<i64>>> input) {
        auto snapshot = std::vector<std::tuple<std::string, i64, i64>>{};
        input.for_each_key_value([&](const auto &key, const auto &value) {
            snapshot.emplace_back(std::string{key.template get<0>().value()},
                                  key.template get<1>().value(), *value);
        });
        std::ranges::sort(snapshot);
        collapsed_snapshots.push_back(std::move(snapshot));
    });
    const auto flip_graph = [&](wp<TS<bool>> select_second, wp<TSD<i64, TS<i64>>> first,
                                wp<TSD<i64, TS<i64>>> second) {
        const auto group = select_group(select_second, first, second);
        inspect_group(select_second, group);
        const auto flipped = operators::flip_keys(make_outer(group));
        observe_flipped(flipped);
        return sum_flipped(flipped);
    };
    const auto collapse_graph = [&](wp<TS<bool>> select_second, wp<TSD<i64, TS<i64>>> first,
                                    wp<TSD<i64, TS<i64>>> second) {
        const auto group = select_group(select_second, first, second);
        inspect_group(select_second, group);
        const auto collapsed = operators::collapse_keys(make_outer(group));
        observe_collapsed(collapsed);
        return sum_collapsed(collapsed);
    };

    CHECK(bg::run_graph(flip_graph, {false, true, literal::SKIP}, {{{1, 5}, {2, 6}}, literal::SKIP, literal::SKIP},
                        {{{2, 60}, {1, 50}}, literal::SKIP, {{2, 61}}})
              .equals<i64>({11, 110, 111}));
    CHECK(bg::run_graph(collapse_graph, {false, true, literal::SKIP},
                        {{{1, 5}, {2, 6}}, literal::SKIP, literal::SKIP},
                        {{{2, 60}, {1, 50}}, literal::SKIP, {{2, 61}}})
              .equals<i64>({11, 110, 111}));
    REQUIRE(remappings.size() == 4);
    CHECK(remappings[0]);
    CHECK_FALSE(remappings[1]);
    CHECK(remappings[2]);
    CHECK_FALSE(remappings[3]);
    CHECK(flipped_snapshots == std::vector<std::vector<std::tuple<i64, std::string, i64>>>{
                                   {{1, "group", 5}, {2, "group", 6}},
                                   {{1, "group", 50}, {2, "group", 60}},
                                   {{1, "group", 50}, {2, "group", 61}}});
    CHECK(collapsed_snapshots == std::vector<std::vector<std::tuple<std::string, i64, i64>>>{
                                     {{"group", 1, 5}, {"group", 2, 6}},
                                     {{"group", 1, 50}, {"group", 2, 60}},
                                     {{"group", 1, 50}, {"group", 2, 61}}});
}

TEST_CASE("uncollapse_keys refreshes remapped items from a referenced dictionary",
          "[feature:operator][feature:reference][series:tsd][feature:collection-change]"
          "[feature:item-id-remapping][binding:reference-to-value][transition:update]") {
    auto snapshots = std::vector<std::vector<std::tuple<i64, std::string, i64>>>{};
    const auto select_source = node(
        [](In<TS<bool>> select_second, In<REF<TSD<Tuple<i64, string>, REF<TS<i64>>>>> first,
           In<REF<TSD<Tuple<i64, string>, REF<TS<i64>>>>> second,
           Out<REF<TSD<Tuple<i64, string>, REF<TS<i64>>>>> output) {
            output.copy(*select_second ? second : first);
        });
    const auto sum = node([](In<TSD<i64, TSD<string, TS<i64>>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &group) {
            group.for_each_value([&](const auto &value) { total += *value; });
        });
        return total;
    });
    const auto observe = node([&](In<TSD<i64, TSD<string, TS<i64>>>> input) {
        auto snapshot = std::vector<std::tuple<i64, std::string, i64>>{};
        input.for_each_key_value([&](const auto &key, const auto &group) {
            group.for_each_key_value([&](const auto &group_key, const auto &value) {
                snapshot.emplace_back(key.value(), std::string{group_key.value()}, *value);
            });
        });
        std::ranges::sort(snapshot);
        snapshots.push_back(std::move(snapshot));
    });
    const auto graph = [&](wp<TS<bool>> select_second, wp<TSD<Tuple<i64, string>, TS<i64>>> first,
                           wp<TSD<Tuple<i64, string>, TS<i64>>> second) {
        const auto uncollapsed = operators::uncollapse_keys(select_source(select_second, first, second));
        observe(uncollapsed);
        return sum(uncollapsed);
    };

    CHECK(bg::run_graph(graph, {false, true, literal::SKIP},
                        {{{{1, "a"}, 5}, {{2, "b"}, 6}}, literal::SKIP, literal::SKIP},
                        {{{{2, "b"}, 60}, {{1, "a"}, 50}}, literal::SKIP, {{{1, "a"}, 51}}})
              .equals<i64>({11, 110, 111}));
    CHECK(snapshots == std::vector<std::vector<std::tuple<i64, std::string, i64>>>{
                           {{1, "a", 5}, {2, "b", 6}},
                           {{1, "a", 50}, {2, "b", 60}},
                           {{1, "a", 51}, {2, "b", 60}}});
}

TEST_CASE("flip inverts unique values and removes a colliding destination with its previous source",
          "[feature:operator][feature:collection-change][series:tsd][transition:removal]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TSD<i64, TS<string>>> input) { return materialize(operators::flip(input)); };

    CHECK(bg::run_graph(graph,
                        bg::input<TSD<i64, TS<string>>>({{{1, "one"}, {2, "two"}},
                                                         {{1, "two"}},
                                                         {{1, literal::REMOVE}}}))
              .equals({{{"one", 1}, {"two", 2}},
                       {{"one", literal::REMOVE}, {"two", 1}},
                       {{"two", literal::REMOVE}}}));
}

TEST_CASE("flip reapplies an unchanged value after a colliding source is removed",
          "[feature:operator][feature:collection-change][series:tsd][transition:removal][transition:update]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TSD<i64, TS<string>>> input) { return materialize(operators::flip(input)); };

    CHECK(bg::run_graph(graph,
                        bg::input<TSD<i64, TS<string>>>({{{1, "same"}, {2, "same"}},
                                                         {{2, literal::REMOVE}, {1, "same"}}}))
              .equals({{{"same", 2}}, {{"same", 1}}}));
}

TEST_CASE("flip removes and restores mappings when a source value becomes invalid",
          "[feature:operator][feature:collection-change][series:tsd][transition:invalidation]"
          "[transition:revalidation]") {
    const auto materialize = node([](In<TSD<string, TS<i64>>> input, Out<TSD<string, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TSD<i64, TS<string>>> input) { return materialize(operators::flip(input)); };

    CHECK(bg::run_graph(graph, bg::input<TSD<i64, TS<string>>>({{{1, "one"}}, {{1, literal::INVALID}}, {{1, "one"}}}))
              .equals({{{"one", 1}}, {{"one", literal::REMOVE}}, {{"one", 1}}}));
}

TEST_CASE("flip groups duplicate values into key sets and removes empty groups",
          "[feature:operator][feature:collection-change][series:tsd][series:tss][transition:removal]") {
    const auto materialize = node([](In<TSD<string, TSS<i64>>> input, Out<TSD<string, TSS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TSD<i64, TS<string>>> input) {
        return materialize(operators::flip(input, false));
    };

    CHECK(bg::run_graph(graph,
                        bg::input<TSD<i64, TS<string>>>({{{1, "shared"}, {2, "shared"}},
                                                         {{1, literal::REMOVE}},
                                                         {{2, literal::REMOVE}}}))
              .equals({{{"shared", {1, 2}}},
                       {{"shared", {literal::REMOVED(1)}}},
                       {{"shared", literal::REMOVE}}}));
}

TEST_CASE("flip options are scalar booleans, not time series inputs",
          "[feature:operator][feature:type-system][series:tsd]") {
    const auto graph = [](wp<TSD<i64, TS<string>>> values, wp<TS<bool>> unique) {
        return operators::flip(values, unique);
    };

    CHECK_THROWS_AS(bg::run_graph(graph, {{{1, "one"}}}, {true}), WiringError);
}
