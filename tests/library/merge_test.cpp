#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/tss/tss_output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace {

template <template <typename, auto...> typename Field> struct MergeBundle {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

template <template <typename, auto...> typename Field> struct FinalMergeBundle {
    Field<TS<i64, TimeSeriesGuarantee::final>> left;
    Field<TS<i64, TimeSeriesGuarantee::final>> right;
};

template <template <typename, auto...> typename Field> struct MixedGuaranteeMergeBundle {
    Field<TS<i64, TimeSeriesGuarantee::final>> final_value;
    Field<TS<i64>> ordinary_value;
};

template <template <typename, auto...> typename Field> struct MixedOrdinaryMergeBundle {
    Field<TS<i64>> final_value;
    Field<TS<i64>> ordinary_value;
};

template <template <typename, auto...> typename Field> struct MergeBundleWithFixedList {
    Field<TSL<TS<i64>, 2>> values;
};

using DictionarySnapshot = std::map<i64, std::optional<i64>>;

struct empty_set_source {
    Out<TSS<i64>> output;

    void start() const { output.clear(); }
};

} // namespace

TEST_CASE("merge selects the leftmost valid input that ticks",
          "[feature:operator][feature:merge][series:value][validity:optional]") {
    const auto graph = [](wp<TS<i64>> left, wp<TS<i64>> right) { return operators::merge(left, right); };

    CHECK(bg::run_graph(graph, std::array<std::optional<i64>, 4>{10, std::nullopt, std::nullopt, 30},
                              std::array<i64, 4>{100, 200, 300, 400})
              .equals<i64>({10, 200, 300, 30}));
}

TEST_CASE("merge selects the leftmost input when ordinary inputs tick together",
          "[feature:merge][series:value][transition:update]") {
    const auto graph = [](wp<TS<i64>> left, wp<TS<i64>> right) { return operators::merge(left, right); };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{10, 11, 12}, std::array<i64, 3>{100, 101, 102})
              .equals<i64>({10, 11, 12}));
}

TEST_CASE("merge forwards equal ordinary ticks",
          "[feature:merge][series:value][guarantee:ordinary][transition:update]") {
    const auto graph = [](wp<TS<i64>> left, wp<TS<i64>> right) { return operators::merge(left, right); };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{10, 10, 10}, std::array<i64, 3>{100, 100, 100})
              .equals<i64>({10, 10, 10}));
}

TEST_CASE("merge copies string values from the selected input",
          "[feature:merge][series:value][scalar:string][validity:optional][transition:update]") {
    const auto graph = [](wp<TS<string>> left, wp<TS<string>> right) { return operators::merge(left, right); };
    const auto left = std::array<std::optional<std::string_view>, 3>{"left", std::nullopt, "left-again"};
    const auto right = std::array<std::string_view, 3>{"right", "right-again", "right-last"};

    CHECK(bg::run_graph(graph, left, right).equals<string>({"left", "right-again", "left-again"}));
}

TEST_CASE("merge preserves a valid scalar value when the selected input invalidates",
          "[feature:merge][series:value][validity:optional][transition:invalidation][transition:update]") {
    const auto left = node([](In<TS<i64>> step, Out<TS<i64>> output) {
        if (*step == 0) {
            output.set(10);
        } else if (*step == 1) {
            output.invalidate();
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TS<i64>> output) {
        if (*step == 0) {
            output.set(100);
        } else if (*step == 2) {
            output.set(200);
        }
    });
    const auto inspect = node([](In<TS<i64>>, In<TS<i64>, passive, optional> value, Out<TS<i64>> output) {
        output.set((value.valid() ? 10'000 : 0) + (value.modified() ? 1'000 : 0) +
                   (value.valid() ? *value : 0));
    });
    const auto graph = [&](wp<TS<i64>> step) {
        return inspect(step, operators::merge(opt(left(step)), opt(right(step))));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({11'010, 10'010, 11'200}));
}

TEST_CASE("merge preserves its last scalar value while inputs are invalid",
          "[feature:merge][series:value][validity:optional][transition:invalidation]"
          "[transition:revalidation]") {
    const auto left = node([](In<TS<i64>> step, Out<TS<i64>> output) {
        if (*step == 0) {
            output.set(10);
        } else if (*step == 1) {
            output.invalidate();
        } else if (*step == 3) {
            output.set(30);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TS<i64>> output) {
        if (*step == 0) {
            output.set(100);
        } else if (*step == 2) {
            output.invalidate();
        }
    });
    const auto observe = node([](In<TS<i64>> step, In<TS<i64>, passive, optional> value, Out<TS<i64>> output) {
        if (value.valid()) {
            output.set(*value);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        return observe(step, operators::merge(opt(left(step)), opt(right(step))));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 4>{0, 1, 2, 3}).equals<i64>({10, 10, 10, 30}));
}

TEST_CASE("merge does not publish before a scalar input first ticks",
          "[feature:merge][series:value][validity:optional][transition:first-tick]") {
    const auto source = node([](In<TS<i64>> step, Out<TS<i64>> output) {
        if (*step == 1) {
            output.set(7);
        }
    });
    const auto never_valid = node([](Out<TS<i64>>) {});
    const auto observe = node([](In<TS<i64>> step, In<TS<i64>, passive, optional> value, Out<TS<i64>> output) {
        if (value.valid()) {
            output.set(*value);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        return observe(step, operators::merge(source(step), never_valid()));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({std::nullopt, 7, 7}));
}

TEST_CASE("merge rejects incompatible scalar inputs",
          "[feature:merge][series:value][scalar:string][scalar:i64]") {
    const auto graph = [](wp<TS<i64>> integer, wp<TS<string>> text) { return operators::merge(integer, text); };
    const auto integer_source = node([](Out<TS<i64>>) {});
    const auto text_source = node([](Out<TS<string>>) {});

    CHECK_THROWS_AS(testing::test_wiring([&] { (void)graph(integer_source(), text_source()); }), WiringError);
}

TEST_CASE("merge rejects scalar-collection mixtures",
          "[feature:merge][series:value][series:tss][shape:multiple]") {
    const auto value = node([](Out<TS<i64>>) {});
    const auto set = node([](Out<TSS<i64>>) {});

    CHECK_THROWS_AS(testing::test_wiring([&] { (void)operators::merge(value(), set()); }), WiringError);
}

TEST_CASE("merge preserves a single input's guarantee",
          "[feature:merge][series:value][guarantee:final][shape:single-node]") {
    const auto source = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(10); });
    const auto graph = [&](wp<TS<i64, TimeSeriesGuarantee::final>> value) {
        const auto result = operators::merge(value);
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        return result.as<TS<i64, TimeSeriesGuarantee::final>>();
    };

    CHECK(bg::run_graph([&] { return graph(source()); }).equals<i64>({10}));
}

TEST_CASE("merge weakens staggered final scalar inputs",
          "[feature:merge][series:value][guarantee:final][guarantee:ordinary][transition:update]") {
    const auto left = node([](In<TS<i64>> step, Out<TS<i64, TimeSeriesGuarantee::final>> output) {
        if (*step == 0) {
            output.set(10);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TS<i64, TimeSeriesGuarantee::final>> output) {
        if (*step == 1) {
            output.set(20);
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(left(step), right(step));
        CHECK(&merged.type() == &TS<i64>::type());
        return merged;
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({10, 20, std::nullopt}));
}

TEST_CASE("merge observes the guarantee of referenced scalar inputs",
          "[feature:merge][series:reference][series:value][guarantee:ordinary][transition:update]") {
    const auto left = node([](In<TS<i64>> step, Out<TS<i64, TimeSeriesGuarantee::final>> output) {
        if (*step == 0) {
            output.set(10);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TS<i64, TimeSeriesGuarantee::final>> output) {
        if (*step == 1) {
            output.set(20);
        }
    });
    const auto reference = node([](In<REF<TS<i64, TimeSeriesGuarantee::final>>> input,
                                   Out<REF<TS<i64, TimeSeriesGuarantee::final>>> output) {
        output.copy(input);
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(reference(left(step)), reference(right(step)));
        CHECK(&merged.type() == &TS<i64>::type());
        return merged;
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({10, 20, std::nullopt}));
}

TEST_CASE("merge weakens staggered final values in fixed TSL inputs",
          "[feature:merge][series:fixed-tsl][guarantee:final][guarantee:ordinary]"
          "[transition:update]") {
    const auto left = node([](In<TS<i64>> step, Out<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>> output) {
        if (*step == 0) {
            output[0].set(10);
        } else if (*step == 2) {
            output[1].set(20);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>> output) {
        if (*step == 1) {
            output[0].set(100);
        } else if (*step == 3) {
            output[1].set(200);
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(left(step), right(step));
        CHECK(&merged.type() == &TSL<TS<i64>, 2>::type());
        return operators::add(operators::getitem(merged, i64{0}), operators::getitem(merged, i64{1}));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 4>{0, 1, 2, 3}).equals<i64>({std::nullopt, std::nullopt, 120, 300}));
}

TEST_CASE("merge observes contained guarantees through references to fixed TSL inputs",
          "[feature:merge][series:reference][series:fixed-tsl][guarantee:final]"
          "[guarantee:ordinary][transition:update]") {
    const auto left = node([](In<TS<i64>> step, Out<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>> output) {
        if (*step == 0) {
            output[0].set(10);
        } else if (*step == 2) {
            output[1].set(20);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>> output) {
        if (*step == 1) {
            output[0].set(100);
        } else if (*step == 3) {
            output[1].set(200);
        }
    });
    const auto reference = node([](In<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>>> input,
                                   Out<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>>> output) {
        output.copy(input);
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(reference(left(step)), reference(right(step))).as<TSL<TS<i64>, 2>>();
        CHECK(merged[0].type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(merged[1].type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return operators::add(merged[0], merged[1]);
    };

    CHECK(bg::run_graph(graph, std::array<i64, 4>{0, 1, 2, 3}).equals<i64>({std::nullopt, std::nullopt, 120, 300}));
}

TEST_CASE("merge weakens staggered final values in TSB fields",
          "[feature:merge][series:tsb][schema:named][guarantee:final][guarantee:ordinary]"
          "[transition:update]") {
    const auto left = node([](In<TS<i64>> step, Out<TSB<FinalMergeBundle>> output) {
        if (*step == 0) {
            output->left.set(10);
        } else if (*step == 2) {
            output->right.set(20);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TSB<FinalMergeBundle>> output) {
        if (*step == 1) {
            output->left.set(100);
        } else if (*step == 3) {
            output->right.set(200);
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(left(step), right(step)).as<TSB<MergeBundle>>();
        CHECK(merged->left.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(merged->right.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return operators::add(merged->left, merged->right);
    };

    CHECK(bg::run_graph(graph, std::array<i64, 4>{0, 1, 2, 3}).equals<i64>({std::nullopt, std::nullopt, 120, 300}));
}

TEST_CASE("merge weakens final fields independently in a mixed-guarantee TSB",
          "[feature:merge][series:tsb][schema:named][guarantee:final][guarantee:ordinary]") {
    const auto left = node([](In<TS<i64>> step, Out<TSB<MixedGuaranteeMergeBundle>> output) {
        if (*step == 0) {
            output->final_value.set(10);
            output->ordinary_value.set(1);
        }
    });
    const auto right = node([](In<TS<i64>> step, Out<TSB<MixedGuaranteeMergeBundle>> output) {
        if (*step == 1) {
            output->final_value.set(20);
            output->ordinary_value.set(2);
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(left(step), right(step)).as<TSB<MixedOrdinaryMergeBundle>>();
        CHECK(merged->final_value.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(merged->ordinary_value.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return operators::add(merged->final_value, merged->ordinary_value);
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({11, 22, std::nullopt}));
}

TEST_CASE("merge combines corresponding items in fixed TSL inputs",
          "[feature:operator][feature:merge][series:fixed-tsl][transition:update]") {
    const auto graph = [](wp<TS<i64>> left_first, wp<TS<i64>> left_second, wp<TS<i64>> right_first,
                          wp<TS<i64>> right_second) {
        const auto left = tsl(left_first, left_second);
        const auto right = tsl(right_first, right_second);
        const auto merged = operators::merge("values"_n = tsl(left, right));
        return operators::add(operators::getitem(merged, i64{0}), operators::getitem(merged, i64{1}));
    };

    CHECK(bg::run_graph(graph, std::array<std::optional<i64>, 3>{1, std::nullopt, 3},
                              std::array<i64, 3>{10, 20, 30}, std::array<i64, 3>{100, 200, 300},
                              std::array<i64, 3>{1000, 2000, 3000})
              .equals<i64>({11, 220, 33}));
}

TEST_CASE("merge dereferences REF items in fixed TSL inputs",
          "[feature:merge][series:fixed-tsl][series:reference][transition:update]") {
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });
    const auto list = node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                              Out<TSL<REF<TS<i64>>, 2>> output) {
        output[0].copy(first);
        output[1].copy(second);
    });
    const auto graph = [&](wp<TS<i64>> left_first, wp<TS<i64>> left_second, wp<TS<i64>> right_first,
                           wp<TS<i64>> right_second) {
        const auto left = list(reference(left_first), reference(left_second));
        const auto right = list(reference(right_first), reference(right_second));
        const auto merged = operators::merge(left, right).as<TSL<TS<i64>, 2>>();
        return operators::add(merged[0], merged[1]);
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{1, 2, 3}, std::array<i64, 3>{10, 20, 30},
                              std::array<i64, 3>{100, 200, 300}, std::array<i64, 3>{1000, 2000, 3000})
              .equals<i64>({11, 22, 33}));
}

TEST_CASE("merge preserves an empty fixed TSL shape",
          "[feature:merge][series:fixed-tsl][shape:source][transition:start]") {
    const auto inspect = node([](In<TSL<TS<i64>, 0>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&] {
        const auto empty = tsl<TS<i64>>();
        return inspect(operators::merge(empty, empty));
    };

    CHECK(bg::run_graph(graph).equals<i64>({0}));
}

TEST_CASE("merge returns the first empty TSL without adding a wiring node",
          "[feature:merge][series:fixed-tsl][shape:single-node]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto first = tsl<TS<i64>>();
    const auto second = tsl<TS<i64>>();
    (void)wiring.complete_pending_wiring();
    const auto node_count = wiring.nodes().size();

    const auto merged = operators::merge(first, second);
    (void)wiring.complete_pending_wiring();

    CHECK(static_cast<WiringPoint>(merged).record() == static_cast<WiringPoint>(first).record());
    CHECK(wiring.nodes().size() == node_count);
}

TEST_CASE("merge recursively combines a fixed TSL field inside a TSB",
          "[feature:merge][series:tsb][series:fixed-tsl][schema:named][shape:nested][transition:update]") {
    const auto make_bundle = node([](In<TS<i64>> step, Out<TSB<MergeBundleWithFixedList>> output) {
        if (*step == 0) {
            output->values[0].set(10);
            output->values[1].set(1);
        } else if (*step == 1) {
            output->values[0].set(20);
        }
    });
    const auto make_other_bundle = node([](In<TS<i64>> step, Out<TSB<MergeBundleWithFixedList>> output) {
        if (*step == 0) {
            output->values[0].set(100);
            output->values[1].set(100);
        } else if (*step == 1) {
            output->values[1].set(200);
        }
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto merged = operators::merge(make_bundle(step), make_other_bundle(step)).as<TSB<MergeBundleWithFixedList>>();
        return operators::add(operators::getitem(merged->values, i64{0}),
                              operators::getitem(merged->values, i64{1}));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 2>{0, 1}).equals<i64>({11, 220}));
}

TEST_CASE("merge combines corresponding fields in TSB inputs",
          "[feature:operator][feature:merge][series:tsb][schema:named][transition:update]") {
    const auto graph = [](wp<TS<i64>> left_first, wp<TS<i64>> left_second, wp<TS<i64>> right_first,
                          wp<TS<i64>> right_second) {
        const auto left = bundle("left"_n = left_first, "right"_n = left_second);
        const auto right = bundle("left"_n = right_first, "right"_n = right_second);
        const auto merged = operators::merge(left, right).as<TSB<MergeBundle>>();
        return operators::add(merged->left, merged->right);
    };

    CHECK(bg::run_graph(graph, std::array<std::optional<i64>, 3>{1, std::nullopt, 3},
                              std::array<i64, 3>{10, 20, 30}, std::array<i64, 3>{100, 200, 300},
                              std::array<i64, 3>{1000, 2000, 3000})
              .equals<i64>({11, 220, 33}));
}

TEST_CASE("merge combines TSD values over the union of keys",
          "[feature:operator][feature:merge][series:tsd][transition:addition][transition:removal]") {
    auto snapshots = std::vector<DictionarySnapshot>{};
    const auto left_dictionary = node([](In<TS<i64>> step, Out<TSD<i64, TS<i64>>> output) {
        switch (*step) {
        case 0:
            output[1].set(10);
            output[2].set(20);
            break;
        case 1:
            output[1].set(11);
            break;
        case 2:
            output.erase(2);
            break;
        }
    });
    const auto right_dictionary = node([](In<TS<i64>> step, Out<TSD<i64, TS<i64>>> output) {
        switch (*step) {
        case 0:
            output[2].set(200);
            output[3].set(300);
            break;
        case 1:
            output[4].set(400);
            break;
        case 3:
            output.erase(2);
            output.erase(3);
            break;
        }
    });
    const auto inspect = node([&](In<TS<i64>>, In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        auto snapshot = DictionarySnapshot{};
        input.for_each_key_value([&](const auto &key, const auto &value) {
            const auto integer_key = scalar_cast<i64>(key).value();
            snapshot.emplace(integer_key, value.valid() ? std::optional<i64>{*value} : std::nullopt);
        });
        snapshots.push_back(std::move(snapshot));
        output.set(static_cast<i64>(snapshots.size()));
    });
    const auto graph = [&](wp<TS<i64>> step) {
        return inspect(step, operators::merge(left_dictionary(step), right_dictionary(step)));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 4>{0, 1, 2, 3}).equals<i64>({1, 2, 3, 4}));
    CHECK(snapshots == std::vector<DictionarySnapshot>{{{1, 10}, {2, 20}, {3, 300}},
                                                       {{1, 11}, {2, 20}, {3, 300}, {4, 400}},
                                                       {{1, 11}, {2, 20}, {3, 300}, {4, 400}},
                                                       {{1, 11}, {4, 400}}});
}

TEST_CASE("merge publishes a valid dictionary when another input is initially invalid",
          "[feature:operator][feature:merge][series:tsd][validity:optional][transition:update]") {
    const auto make_values = node([](In<TS<i64>> trigger, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(trigger.value());
    });
    const auto empty_values = node([](In<TS<i64>>, Out<TSD<i64, TS<i64>>>) {});
    const auto to_reference = node([](In<TSD<i64, REF<TS<i64>>>, optional> input,
                                      Out<TSD<i64, REF<TS<i64>>>> output) { output.copy_tick(input); });

    auto direct_merge_reached = false;
    auto normalized_merge_reached = false;
    const auto observe_direct = node([&](In<TSD<i64, TS<i64>>> values) {
        values.for_each_added([&](const Scalar<i64> &key) { direct_merge_reached |= values[key].valid(); });
        values.for_each_modified(
            [&](const Scalar<i64> &key) { direct_merge_reached |= values[key].valid(); });
    });
    const auto observe_normalized_merge = node([&](In<TSD<i64, TS<i64>>> values) {
        values.for_each_added([&](const Scalar<i64> &key) { normalized_merge_reached |= values[key].valid(); });
        values.for_each_modified(
            [&](const Scalar<i64> &key) { normalized_merge_reached |= values[key].valid(); });
    });
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto values = make_values(trigger);
        const auto empty = empty_values(trigger);
        const auto references = to_reference(values);
        const auto empty_references = to_reference(empty);
        observe_direct(operators::merge(values, empty));
        observe_normalized_merge(operators::merge(references, empty_references));
        return trigger;
    };

    (void)bg::run_graph(graph, {42});

    CHECK(direct_merge_reached);
    CHECK(normalized_merge_reached);
}

TEST_CASE("merge combines TSS membership and retains keys present in another input",
          "[feature:operator][feature:merge][series:tss][transition:addition][transition:removal]") {
    auto snapshots = std::vector<std::vector<i64>>{};
    const auto left_set = node([](In<TS<i64>> step, Out<TSS<i64>> output) {
        switch (*step) {
        case 0:
            output.insert(1);
            output.insert(2);
            break;
        case 1:
            output.erase(2);
            break;
        }
    });
    const auto right_set = node([](In<TS<i64>> step, Out<TSS<i64>> output) {
        switch (*step) {
        case 0:
            output.insert(2);
            output.insert(3);
            break;
        case 2:
            output.erase(2);
            output.erase(3);
            break;
        }
    });
    const auto inspect = node([&](In<TSS<i64>> input, Out<TS<i64>> output) {
        auto snapshot = std::vector<i64>{};
        input.for_each([&](const auto &key) { snapshot.push_back(scalar_cast<i64>(key).value()); });
        std::ranges::sort(snapshot);
        snapshots.push_back(std::move(snapshot));
        output.set(static_cast<i64>(snapshots.size()));
    });
    const auto graph = [&](wp<TS<i64>> step) { return inspect(operators::merge(left_set(step), right_set(step))); };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, std::nullopt, 2}));
    CHECK(snapshots == std::vector<std::vector<i64>>{{1, 2, 3}, {1}});
}

TEST_CASE("merge retains a TSS key transferred between inputs in one cycle",
          "[feature:merge][series:tss][transition:addition][transition:removal]") {
    const auto left_set = node([](In<TS<i64>> step, Out<TSS<i64>> output) {
        if (*step == 0) {
            output.insert(2);
        } else if (*step == 1) {
            output.erase(2);
        }
    });
    const auto right_set = node([](In<TS<i64>> step, Out<TSS<i64>> output) {
        if (*step == 1) {
            output.insert(2);
        } else if (*step == 2) {
            output.erase(2);
        }
    });
    auto changes = std::vector<bool>{};
    const auto inspect = node([&](In<TS<i64>> step, In<TSS<i64>> input, Out<TS<i64>> output) {
        changes.push_back(input.modified());
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TS<i64>> step) {
        return inspect(step, operators::merge(left_set(step), right_set(step)));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, 1, 0}));
    CHECK(changes == std::vector<bool>{true, false, true});
}

TEST_CASE("merge keeps TSS output invalid until an input becomes valid",
          "[feature:merge][series:tss][validity:optional][transition:revalidation]") {
    const auto populated_set = node([](Out<TSS<i64>> output) { output.insert(1); });
    const auto invalid_set = node([](Out<TSS<i64>>) {});
    const auto select = node([](In<TS<i64>> step, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*step == 0 || *step == 2 ? invalid : populated);
    });
    const auto inspect = node([](In<TS<i64>>, In<TSS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? 100 + static_cast<i64>(input.size()) : -1);
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto selected = select(step, populated_set(), invalid_set());
        return inspect(step, operators::merge(selected, selected));
    };

    const auto actual = bg::run_graph(graph, std::array<i64, 3>{0, 1, 2});
    CAPTURE(actual.size());
    CHECK(actual.size() == 3);
    CHECK(actual.equals<i64>({-1, 101, 100}));
}

TEST_CASE("merge publishes a valid empty TSS when an input is valid and empty",
          "[feature:merge][series:tss][validity:optional][shape:source][transition:start]") {
    const auto invalid_set = node([](Out<TSS<i64>>) {});
    const auto inspect = node([](In<TSS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? 100 + static_cast<i64>(input.size()) : -1);
    });
    const auto empty_set = node<empty_set_source>();
    const auto graph = [&] { return inspect(operators::merge(invalid_set(), empty_set())); };

    CHECK(bg::run_graph(graph).equals<i64>({100}));
}

TEST_CASE("merge removes keys from an invalid reference while another TSS remains valid",
          "[feature:merge][series:tss][series:reference][validity:optional]"
          "[transition:invalidation]") {
    const auto populated_set = node([](In<TS<i64>> step, Out<TSS<i64>> output) {
        if (*step == 0) {
            output.insert(1);
        }
    });
    const auto invalid_set = node([](Out<TSS<i64>>) {});
    const auto select = node([](In<TS<i64>> step, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*step == 1 ? invalid : populated);
    });
    const auto empty_set = node<empty_set_source>();
    const auto inspect = node([](In<TS<i64>> step, In<TSS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? static_cast<i64>(input.size()) : -1);
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto selected = select(step, populated_set(step), invalid_set());
        return inspect(step, operators::merge(selected, empty_set()));
    };

    CHECK(bg::run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, 0, 1}));
}

TEST_CASE("merge rejects no inputs and preserves one-input identity behavior",
          "[feature:operator][feature:merge][shape:single-node]") {
    CHECK_THROWS_AS(testing::test_wiring([] { (void)operators::merge(); }), WiringError);

    const auto identity = [](wp<TS<i64>> value) { return operators::merge(value); };
    CHECK(bg::run_graph(identity, std::array<i64, 2>{4, 5}).equals<i64>({4, 5}));
}

TEST_CASE("merge returns one input without adding a wiring node",
          "[feature:merge][shape:single-node]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source = node([](Out<TS<i64>>) {});
    const auto value = source();
    (void)wiring.complete_pending_wiring();
    const auto node_count = wiring.nodes().size();

    const auto merged = operators::merge(value);
    (void)wiring.complete_pending_wiring();

    CHECK(static_cast<WiringPoint>(merged).record() == static_cast<WiringPoint>(value).record());
    CHECK(wiring.nodes().size() == node_count);
}

TEST_CASE("merge forwards one input invalidation",
          "[feature:merge][series:value][validity:optional][transition:invalidation]") {
    const auto source = node([](In<TS<i64>> step, Out<TS<i64>> output) {
        if (*step == 0) {
            output.set(4);
        } else if (*step == 1) {
            output.invalidate();
        } else if (*step == 2) {
            output.set(6);
        }
    });
    const auto identity = [&](wp<TS<i64>> step) { return operators::valid(operators::merge(source(step))); };

    CHECK(bg::run_graph(identity, std::array<i64, 3>{0, 1, 2}).equals<bool>({true, false, true}));
}
