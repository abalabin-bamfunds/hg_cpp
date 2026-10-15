#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace bg;

namespace stream_test {
struct non_comparable_value {
    i64 value;
};

template <template <typename, auto...> typename Field> struct stream_operator_bundle {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

template <template <typename, auto...> typename Field> struct mixed_guarantee_bundle {
    Field<TS<i64>> changing;
    Field<TS<i64, TimeSeriesGuarantee::const_>> constant;
};

} // namespace stream_test

namespace {

struct delayed_value_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct changing_item_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if ((*step)++ == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.set(2);
    }
};

struct empty_dictionary_source {
    Out<TSD<i64, TS<i64>>> output;

    void start() const { output.clear(); }
};

struct changing_set_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSS<i64>> output;

    void start(EvaluationContext *context) const {
        output.clear();
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if ((*step)++ == 0) {
            output.insert(1);
            output.insert(2);
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.erase(2);
    }
};

struct deduplicated_source {
    Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output;

    void start() const { output.set(1); }
};

struct stable_value_source {
    Out<TS<i64>> output;

    void start() const { output.set(10); }
    void evaluate() const {}
};

struct first_equal_value_source {
    Out<TS<i64>> output;

    void start() const { output.set(10); }
};

struct second_equal_value_source {
    Out<TS<i64>> output;

    void start() const { output.set(10); }
};

struct changing_bundle_source {
    In<TS<i64>> step;
    Out<TSB<stream_test::mixed_guarantee_bundle, TimeSeriesGuarantee::deduplicated>> output;

    void evaluate() const {
        if (*step == 0) {
            output->changing.set(10);
            output->constant.set(1);
        } else if (*step == 1) {
            output->changing.set(20);
        }
    }
};

} // namespace

TEST_CASE("valid publishes the input validity", "[feature:operator][feature:validity][validity:optional][series:value]") {
    const auto source = node<delayed_value_source>();
    const auto query = [&](wp<TS<i64>> value) { return operators::valid(value); };
    const auto graph = [&] { return query(source()); };

    CHECK(testing::test_graph(graph).equals<bool>({false, true}));
}

TEST_CASE("valid observes target validity through a reference",
          "[feature:operator][feature:validity][series:reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_item_source>();
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });
    const auto graph = [&] { return operators::valid(reference(source())); };

    CHECK(testing::test_graph(graph).equals<bool>({true, false, true}));
}

TEST_CASE("all_valid checks every item in a fixed list",
          "[feature:operator][feature:validity][validity:all-valid][series:fixed-tsl]") {
    const auto query = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::all_valid(tsl(first, second));
    };

    CHECK(testing::test_graph(query, std::array<i64, 2>{1, 2}, std::array<i64, 2>{3, 4})
              .equals<bool>({true, std::nullopt}));
}

TEST_CASE("all_valid tracks immediate item invalidation in a free list",
          "[feature:operator][feature:validity][validity:all-valid][series:fixed-tsl][representation:free]"
          "[transition:invalidation][transition:revalidation]") {
    const auto changing_item = node<changing_item_source>();
    const auto graph = [&](wp<TS<i64>> first) { return operators::all_valid(tsl(first, changing_item())); };

    CHECK(testing::test_graph(graph, std::array<i64, 1>{10}).equals<bool>({true, false, true}));
}

TEST_CASE("all_valid treats a valid empty dictionary as valid",
          "[feature:operator][feature:validity][validity:all-valid][series:tsd]") {
    const auto source = node<empty_dictionary_source>();
    const auto query = [](wp<TSD<i64, TS<i64>>> value) { return operators::all_valid(value); };
    const auto graph = [&] { return query(source()); };

    CHECK(testing::test_graph(graph).equals<bool>({true}));
}

TEST_CASE("sample copies the current value on every signal tick",
          "[feature:operator][series:value][shape:chain]") {
    const auto sample = [](wp<TS<i64>> signal, wp<TS<i64>> value) { return operators::sample(signal, value); };

    CHECK(testing::test_graph(sample, std::array<i64, 3>{0, 1, 2}, std::array<i64, 3>{10, 20, 30})
              .equals<i64>({10, 20, 30}));
}

TEST_CASE("filter preserves the last value while closed and skips an unchanged value on reopening",
          "[feature:operator][series:value][validity:optional]") {
    const auto source = node<stable_value_source>();
    const auto filter = [&](wp<TS<bool>> condition) { return operators::filter_(condition, source()); };

    CHECK(testing::test_graph(filter, std::array{true, false, true, true})
              .equals<i64>({10, std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("filter copies scalar input invalidation while open",
          "[feature:operator][feature:validity][series:value][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_item_source>();
    const auto filter = [&](wp<TS<bool>> condition) {
        return operators::filter_(condition, source());
    };

    CHECK(testing::test_graph(filter, std::array{true}).equals<i64>({1, std::nullopt, 2}));
}

TEST_CASE("filter invalidates its output when an open input becomes invalid",
          "[feature:operator][feature:validity][series:value][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_item_source>();
    const auto observe_validity = node([](In<TS<i64>, optional> value, Out<TS<bool>> output) {
        output.set(value.valid());
    });
    const auto filter = [&](wp<TS<bool>> condition) {
        return observe_validity(operators::filter_(condition, source()));
    };

    CHECK(testing::test_graph(filter, std::array{true}).equals<bool>({true, false, true}));
}

TEST_CASE("filter reconciles a fixed list when it reopens",
          "[feature:operator][series:fixed-tsl][validity:optional][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return sum(operators::filter_(condition, tsl(value, operators::const_(i64{1}))));
    };

    CHECK(testing::test_graph(filter, std::array{true, false, true, true}, std::array<i64, 4>{10, 20, 30, 40})
              .equals<i64>({11, std::nullopt, 31, 41}));
}

TEST_CASE("filter limits contained guarantees by the input root guarantee",
          "[feature:operator][series:tsb][schema:named][guarantee:ordinary]"
          "[validity:optional][transition:update]") {
    auto constant_item_updates = std::vector<bool>{};
    const auto observe = node([&](In<TSB<SCHEMA>> values, Out<TS<i64>> output) {
        constant_item_updates.push_back(values.item_updated(1));
        output.set(*values[0].template as<TS<i64>>());
    });
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> ordinary_value) {
        const auto values = bundle<stream_test::mixed_guarantee_bundle>(ordinary_value, operators::const_(i64{1}));
        const auto filtered = operators::filter_(condition, values);
        const auto *type = type_cast<TimeSeriesBundleType>(&filtered.type());
        REQUIRE(type != nullptr);
        CHECK(filtered.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(&type->item_type(0) == &TS<i64>::type());
        CHECK(&type->item_type(1) == &TS<i64>::type());
        return observe(filtered);
    };

    const auto actual = testing::test_graph(filter, std::array{true, false, true, true},
                                            std::array<i64, 4>{10, 20, 30, 40});
    CHECK(actual.equals<i64>({10, std::nullopt, 30, 40}));
    CHECK(constant_item_updates == std::vector<bool>{true, true, false});
}

TEST_CASE("filter caps a stronger child at a deduplicated root",
          "[feature:operator][series:tsb][schema:named][guarantee:deduplicated]"
          "[transition:update]") {
    auto constant_item_updates = std::vector<bool>{};
    const auto source = node<changing_bundle_source>();
    const auto observe = node([&](In<TSB<SCHEMA>> values, Out<TS<i64>> output) {
        constant_item_updates.push_back(values.item_updated(1));
        output.set(*values[0].template as<TS<i64>>());
    });
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> step) {
        const auto filtered = operators::filter_(condition, source(step));
        const auto *type = type_cast<TimeSeriesBundleType>(&filtered.type());
        REQUIRE(type != nullptr);
        CHECK(filtered.type().guarantee().value() == TimeSeriesGuarantee::deduplicated);
        CHECK(&type->item_type(0) == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&type->item_type(1) == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        return observe(filtered);
    };

    CHECK(testing::test_graph(filter, std::array{true, false, true}, std::array<i64, 3>{0, 1, 2})
              .equals<i64>({10, std::nullopt, 20}));
    CHECK(constant_item_updates == std::vector<bool>{true, false});
}

TEST_CASE("filter reconciles a TSB when it reopens",
          "[feature:operator][series:tsb][schema:named][validity:optional][transition:update]") {
    const auto sum = node([](In<TSB<stream_test::stream_operator_bundle>> input, Out<TS<i64>> output) {
        output.set(*input->left + *input->right);
    });
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return sum(operators::filter_(
            condition, bundle<stream_test::stream_operator_bundle>(value, operators::const_(i64{1}))));
    };

    CHECK(testing::test_graph(filter, std::array{true, false, true, true}, std::array<i64, 4>{10, 20, 30, 40})
              .equals<i64>({11, std::nullopt, 31, 41}));
}

TEST_CASE("filter reflects TSS membership changes made while closed",
          "[feature:operator][series:tss][validity:optional][transition:removal][transition:addition]") {
    auto snapshots = std::vector<std::vector<i64>>{};
    const auto make_set = node([](In<TS<i64>> step, Out<TSS<i64>> output) {
        switch (*step) {
        case 0:
            output.clear();
            output.insert(1);
            output.insert(2);
            break;
        case 1:
            output.erase(2);
            break;
        case 2:
            output.insert(3);
            break;
        case 4:
            output.erase(1);
            output.insert(4);
            break;
        case 5:
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
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> step) {
        return inspect(operators::filter_(condition, make_set(step)));
    };

    const auto condition = std::array{true, false, false, true, true};
    const auto step = std::array<i64, 5>{0, 1, 2, 3, 4};
    CHECK(testing::test_graph(filter, condition, step).equals<i64>({1, std::nullopt, std::nullopt, 2, 3}));
    CHECK(snapshots == std::vector<std::vector<i64>>{{1, 2}, {1, 3}, {3, 4}});
}

TEST_CASE("filter reflects TSD key changes made while closed and forwards open changes",
          "[feature:operator][series:tsd][validity:optional][transition:removal]"
          "[transition:addition][transition:update]") {
    auto snapshots = std::vector<std::map<i64, std::optional<i64>>>{};
    const auto make_dictionary = node([](In<TS<i64>> step, Out<TSD<i64, TS<i64>>> output) {
        switch (*step) {
        case 0:
            output.clear();
            output[1].set(10);
            output[2].set(20);
            break;
        case 1:
            output[1].invalidate();
            output.erase(2);
            break;
        case 2:
            output[3].set(30);
            break;
        case 4:
            output[1].set(11);
            output[4].set(40);
            break;
        case 5:
            output[2].set(22);
            break;
        }
    });
    const auto inspect = node([&](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        auto snapshot = std::map<i64, std::optional<i64>>{};
        input.for_each_key_value([&](const auto &key, const auto &value) {
            const auto integer_key = scalar_cast<i64>(key).value();
            if (value.valid()) {
                snapshot.emplace(integer_key, static_cast<i64>(*value));
            } else {
                snapshot.emplace(integer_key, std::nullopt);
            }
        });
        snapshots.push_back(std::move(snapshot));
        output.set(static_cast<i64>(snapshots.size()));
    });
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> step) {
        return inspect(operators::filter_(condition, make_dictionary(step)));
    };

    const auto condition = std::array{true, false, false, true, true, true};
    const auto step = std::array<i64, 6>{0, 1, 2, 3, 4, 5};
    CHECK(testing::test_graph(filter, condition, step).equals<i64>({1, std::nullopt, std::nullopt, 2, 3, 4}));
    CHECK(snapshots ==
          std::vector<std::map<i64, std::optional<i64>>>{{{1, 10}, {2, 20}},
                                                         {{1, std::nullopt}, {3, 30}},
                                                         {{1, 11}, {3, 30}, {4, 40}},
                                                         {{1, 11}, {2, 22}, {3, 30}, {4, 40}}});
}

TEST_CASE("filter reopens with a recreated final TSD item",
          "[feature:operator][series:tsd][guarantee:final][validity:optional]"
          "[transition:removal][transition:revalidation]") {
    const auto make_dictionary = node([](In<TS<i64>> step, Out<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>> output) {
        switch (*step) {
        case 0:
            output[1].set(10);
            break;
        case 1:
            output.erase(1);
            break;
        case 2:
            output[1].set(20);
            break;
        }
    });
    const auto inspect = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        if (input.contains(1)) {
            output.set(*input[1]);
        }
    });
    const auto filter = [&](wp<TS<bool>> condition, wp<TS<i64>> step) {
        const auto filtered = operators::filter_(condition, make_dictionary(step));
        CHECK(&filtered.type() == &TSD<i64, TS<i64>>::type());
        return inspect(filtered);
    };

    CHECK(testing::test_graph(filter, std::array{true, false, false, true}, std::array<i64, 4>{0, 1, 2, 3})
              .equals<i64>({10, std::nullopt, std::nullopt, 20}));
}

TEST_CASE("filter restores TSS after a reference switches through an invalid target",
          "[feature:operator][series:tss][series:reference][validity:optional]"
          "[transition:invalidation][transition:rebind]") {
    const auto make_set = node([](In<TS<i64>> cycle, Out<TSS<i64>> output) {
        if (*cycle == 0) {
            output.insert(1);
        }
    });
    const auto invalid_set = node([](Out<TSS<i64>>) {});
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*cycle == 1 ? invalid : populated);
    });
    const auto inspect = node([](In<TSS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? static_cast<i64>(input.size()) : -1);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto selected = select(cycle, make_set(cycle), invalid_set());
        return inspect(operators::filter_(operators::const_(true), selected));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, 0, 1}));
}

TEST_CASE("filter restores TSD after a reference switches through an invalid target",
          "[feature:operator][series:tsd][series:reference][validity:optional]"
          "[transition:invalidation][transition:rebind]") {
    const auto make_dictionary = node([](In<TS<i64>> cycle, Out<TSD<i64, TS<i64>>> output) {
        if (*cycle == 0) {
            output[1].set(10);
        }
    });
    const auto invalid_dictionary = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSD<i64, TS<i64>>>, passive> populated,
                                In<REF<TSD<i64, TS<i64>>>, passive> invalid,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*cycle == 1 ? invalid : populated);
    });
    const auto inspect = node([](In<TSD<i64, TS<i64>>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto selected = select(cycle, make_dictionary(cycle), invalid_dictionary());
        return inspect(operators::filter_(operators::const_(true), selected));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, 0, 1}));
}

TEST_CASE("filter copies an invalid TSS target when it reopens",
          "[feature:operator][series:tss][series:reference][validity:optional]"
          "[transition:invalidation][transition:rebind]") {
    const auto make_set = node([](In<TS<i64>> cycle, Out<TSS<i64>> output) {
        if (*cycle == 0) {
            output.insert(1);
        }
    });
    const auto invalid_set = node([](Out<TSS<i64>>) {});
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*cycle == 0 ? populated : invalid);
    });
    const auto inspect = node([](In<TSS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? static_cast<i64>(input.size()) : -1);
    });
    const auto graph = [&](wp<TS<bool>> condition, wp<TS<i64>> cycle) {
        const auto selected = select(cycle, make_set(cycle), invalid_set());
        return inspect(operators::filter_(condition, selected));
    };

    CHECK(testing::test_graph(graph, std::array{true, false, true}, std::array<i64, 3>{0, 1, 2})
              .equals<i64>({1, std::nullopt, 0}));
}

TEST_CASE("filter copies an invalid TSD target when it reopens",
          "[feature:operator][series:tsd][series:reference][validity:optional]"
          "[transition:invalidation][transition:rebind]") {
    const auto make_dictionary = node([](In<TS<i64>> cycle, Out<TSD<i64, TS<i64>>> output) {
        if (*cycle == 0) {
            output[1].set(10);
        }
    });
    const auto invalid_dictionary = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSD<i64, TS<i64>>>, passive> populated,
                                In<REF<TSD<i64, TS<i64>>>, passive> invalid,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*cycle == 0 ? populated : invalid);
    });
    const auto inspect = node([](In<TSD<i64, TS<i64>>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TS<bool>> condition, wp<TS<i64>> cycle) {
        const auto selected = select(cycle, make_dictionary(cycle), invalid_dictionary());
        return inspect(operators::filter_(condition, selected));
    };

    CHECK(testing::test_graph(graph, std::array{true, false, true}, std::array<i64, 3>{0, 1, 2})
              .equals<i64>({1, std::nullopt, 0}));
}

TEST_CASE("filter caps const guarantees recursively at final",
          "[feature:operator][series:value][guarantee:const][guarantee:final]") {
    const auto source = node([](Out<TSL<TS<i64, TimeSeriesGuarantee::const_>, 2, TimeSeriesGuarantee::const_>> output) {
        output[0].set(10);
        output[1].set(20);
    });
    const auto sum = node([](In<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>> value, Out<TS<i64>> output) {
        output.set(*value[0] + *value[1]);
    });
    const auto graph = [&](wp<TS<bool>> condition) {
        const auto result = operators::filter_(condition, source());
        CHECK(&result.type() ==
              &TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>::type());
        return sum(result);
    };

    CHECK(testing::test_graph(graph, std::array{false, true}).equals<i64>({std::nullopt, 30}));

    const auto final_source = node([](Out<TSL<TS<i64, TimeSeriesGuarantee::final>, 2,
                                                    TimeSeriesGuarantee::final>> output) {
        output[0].set(10);
        output[1].set(20);
    });
    const auto final_graph = [&](wp<TS<bool>> condition) {
        const auto result = operators::filter_(condition, final_source());
        CHECK(&result.type() ==
              &TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>::type());
        return sum(result);
    };
    CHECK(testing::test_graph(final_graph, std::array{false, true}).equals<i64>({std::nullopt, 30}));
}

TEST_CASE("if_then_else switches references and ignores repeated selections",
          "[feature:operator][series:reference][guarantee:deduplicated]") {
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto select = [&](wp<TS<bool>> condition) {
        return copy(operators::if_then_else(condition, operators::const_(i64{10}), operators::const_(i64{20})));
    };

    CHECK(testing::test_graph(select, std::array{true, true, false, false, true})
              .equals<i64>({10, std::nullopt, 20, std::nullopt, 10}));
}

TEST_CASE("if_then_else follows the selected value and ignores unselected value changes",
          "[feature:operator][series:reference]") {
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto select = [&](wp<TS<bool>> condition, wp<TS<i64>> true_value, wp<TS<i64>> false_value) {
        return copy(operators::if_then_else(condition, true_value, false_value));
    };

    CHECK(testing::test_graph(select, std::array{true, true, true}, std::array<i64, 3>{10, 20, 30},
                              std::array<i64, 3>{1, 2, 3})
              .equals<i64>({10, 20, 30}));
}

TEST_CASE("if_then_else ignores unselected target updates while the selected target is unchanged",
          "[feature:operator][series:reference][transition:update]") {
    const auto selected = node<stable_value_source>();
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto select = [&](wp<TS<i64>> unselected_value) {
        return copy(operators::if_then_else(operators::const_(true), selected(), unselected_value));
    };

    CHECK(testing::test_graph(select, std::array<i64, 3>{1, 2, 3}).equals<i64>({10, std::nullopt, std::nullopt}));
}

TEST_CASE("if_then_else ignores retargeting of the unselected reference",
          "[feature:operator][series:reference][binding:reference-to-reference]") {
    const auto select_reference = node([](In<TS<bool>> condition, In<REF<TS<i64>>> true_value,
                                          In<REF<TS<i64>>> false_value, Out<REF<TS<i64>>> output) {
        output.copy(*condition ? true_value : false_value);
    });
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto select = [&](wp<TS<bool>> condition, wp<TS<bool>> route) {
        const auto unused = select_reference(route, operators::const_(i64{20}), operators::const_(i64{30}));
        return copy(operators::if_then_else(condition, operators::const_(i64{10}), unused));
    };

    CHECK(testing::test_graph(select, std::array{true, true, true}, std::array{false, true, false})
              .equals<i64>({10, std::nullopt, std::nullopt}));
}

TEST_CASE("if_then_else follows retargeting of the selected reference",
          "[feature:operator][series:reference][binding:reference-to-reference]") {
    const auto select_reference = node([](In<TS<bool>> condition, In<REF<TS<i64>>> true_value,
                                          In<REF<TS<i64>>> false_value, Out<REF<TS<i64>>> output) {
        output.copy(*condition ? true_value : false_value);
    });
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto select = [&](wp<TS<bool>> route) {
        const auto selected = select_reference(route, operators::const_(i64{10}), operators::const_(i64{20}));
        return copy(operators::if_then_else(operators::const_(true), selected, operators::const_(i64{30})));
    };

    CHECK(testing::test_graph(select, std::array{true, false, true}).equals<i64>({10, 20, 10}));
}

TEST_CASE("if_then_else invalidates for an invalid condition and an invalid selected branch",
          "[feature:operator][series:reference][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto copy = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) { output.copy(input); });
    const auto condition_source = node([](In<TS<i64>> cycle, Out<TS<bool>> output) {
        if (*cycle == 1) {
            output.invalidate();
        } else {
            output.set(true);
        }
    });
    const auto invalid_source = node([](Out<TS<i64>>) {});
    const auto invalid_condition = [&](wp<TS<i64>> cycle) {
        return copy(operators::if_then_else(condition_source(cycle), operators::const_(i64{10}),
                                            operators::const_(i64{20})));
    };
    const auto invalid_branch = [&](wp<TS<bool>> condition) {
        return copy(operators::if_then_else(condition, operators::const_(i64{10}), invalid_source()));
    };

    CHECK(testing::test_graph(invalid_condition, std::array<i64, 3>{0, 1, 2})
              .equals<i64>({10, std::nullopt, 10}));
    CHECK(testing::test_graph(invalid_branch, std::array{true, false, true})
              .equals<i64>({10, std::nullopt, 10}));
}

TEST_CASE("if_then_else observes selection changes between equal-valued targets",
          "[feature:operator][series:reference][binding:reference-to-reference]") {
    const auto first = node<first_equal_value_source>();
    const auto second = node<second_equal_value_source>();
    auto previous = Reference{};
    const auto observe_selection = node([&](In<REF<TS<i64>>> selected, Out<TS<bool>> output) {
        output.set(selected.value() != previous);
        previous = selected.value();
    });
    const auto select = [&](wp<TS<bool>> condition) {
        return observe_selection(operators::if_then_else(condition, first(), second()));
    };

    CHECK(testing::test_graph(select, std::array{true, false, true}).equals<bool>({true, true, true}));
}

TEST_CASE("if_then_else selects TSS values",
          "[feature:operator][series:tss][series:reference][transition:update]") {
    const auto make_set = node([](In<TS<i64>> key, Out<TSS<i64>> output) {
        output.clear();
        output.insert(*key);
    });
    const auto sum = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        auto result = i64{};
        input.for_each([&](const auto &key) { result += scalar_cast<i64>(key).value(); });
        output.set(result);
    });
    const auto select = [&](wp<TS<bool>> condition, wp<TS<i64>> left, wp<TS<i64>> right) {
        return sum(operators::if_then_else(condition, make_set(left), make_set(right)));
    };

    CHECK(testing::test_graph(select, std::array{true, false, true}, std::array<i64, 3>{1, 2, 3},
                              std::array<i64, 3>{10, 20, 30})
              .equals<i64>({1, 20, 3}));
}

TEST_CASE("if_then_else follows sparse TSD updates on the selected branch",
          "[feature:operator][series:tsd][series:reference][transition:update]") {
    const auto make_dictionary = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*value);
    });
    const auto read = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) { output.set(*input[1]); });
    const auto select = [&](wp<TS<bool>> condition, wp<TS<i64>> left, wp<TS<i64>> right) {
        return read(operators::if_then_else(condition, make_dictionary(left), make_dictionary(right)));
    };
    const auto condition = std::array<std::optional<bool>, 4>{true, std::nullopt, false, std::nullopt};
    const auto left = std::array<std::optional<i64>, 4>{10, std::nullopt, 30, std::nullopt};
    const auto right = std::array<std::optional<i64>, 4>{20, 21, 22, 23};

    CHECK(testing::test_graph(select, condition, left, right).equals<i64>({10, std::nullopt, 22, 23}));
}

TEST_CASE("if_then_else selects a whole fixed list",
          "[feature:operator][series:fixed-tsl][series:reference][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto select = [&](wp<TS<bool>> condition, wp<TS<i64>> left, wp<TS<i64>> right) {
        return sum(operators::if_then_else(condition, tsl(left, operators::const_(i64{100})),
                                           tsl(operators::const_(i64{200}), right)));
    };

    CHECK(testing::test_graph(select, std::array{true, false, true}, std::array<i64, 3>{1, 2, 3},
                              std::array<i64, 3>{10, 20, 30})
              .equals<i64>({101, 220, 103}));
}

TEST_CASE("if_then_else selects a whole TSB",
          "[feature:operator][series:tsb][series:reference][schema:named][transition:update]") {
    const auto sum = node([](In<TSB<stream_test::stream_operator_bundle>> input, Out<TS<i64>> output) {
        output.set(*input->left + *input->right);
    });
    const auto select = [&](wp<TS<bool>> condition, wp<TS<i64>> left, wp<TS<i64>> right) {
        return sum(operators::if_then_else(
            condition, bundle<stream_test::stream_operator_bundle>(left, operators::const_(i64{100})),
            bundle<stream_test::stream_operator_bundle>(operators::const_(i64{200}), right)));
    };

    CHECK(testing::test_graph(select, std::array{true, false, true}, std::array<i64, 3>{1, 2, 3},
                              std::array<i64, 3>{10, 20, 30})
              .equals<i64>({101, 220, 103}));
}

TEST_CASE("sample uses the latest passive value and preserves output on invalidation",
          "[feature:operator][feature:validity][series:value][activity:passive]"
          "[transition:invalidation][transition:revalidation]") {
    const auto invalidate_on_negative = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input < 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto sample = [&](wp<TS<i64>> signal, wp<TS<i64>> value) {
        return operators::sample(signal, invalidate_on_negative(value));
    };

    CHECK(testing::test_graph(sample, std::array<i64, 4>{0, 1, 2, 3}, std::array<i64, 4>{10, -1, 20, 30})
              .equals<i64>({10, std::nullopt, 20, 30}));

    const auto retain_passive_value = [](wp<TS<i64>> signal, wp<TS<i64>> value) {
        return operators::sample(signal, value);
    };
    const auto sparse_signal = std::array<std::optional<i64>, 5>{0, std::nullopt, 2, std::nullopt, 4};
    const auto sparse_value = std::array<std::optional<i64>, 5>{10, 20, std::nullopt, 30, std::nullopt};
    CHECK(testing::test_graph(retain_passive_value, sparse_signal, sparse_value)
              .equals<i64>({10, std::nullopt, 20, std::nullopt, 30}));
}

TEST_CASE("sample observes a reference target only on signal ticks",
          "[feature:operator][feature:sampling][series:reference][activity:passive]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_item_source>();
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });
    const auto sample = [&](wp<TS<i64>> signal) {
        return operators::sample(signal, reference(source()));
    };

    CHECK(testing::test_graph(sample, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, std::nullopt, 2}));
}

TEST_CASE("sample limits stronger input guarantees to ordinary",
          "[feature:operator][feature:type-system][series:value][guarantee:const]") {
    const auto graph = [](wp<TS<i64>> signal) {
        const auto result = operators::sample(signal, operators::const_(i64{42}));
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };

    CHECK(testing::test_graph(graph, std::array<i64, 2>{0, 1}).equals<i64>({42, 42}));
}

TEST_CASE("sample republishes const list items on every signal tick",
          "[feature:operator][series:fixed-tsl][guarantee:const][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>, all_valid> values, Out<TS<i64>> output) {
        output.set(*values[0] + *values[1]);
    });
    const auto graph = [&](wp<TS<i64>> signal) {
        const auto values = tsl(operators::const_(i64{10}), operators::const_(i64{20}));
        return sum(operators::sample(signal, values));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 2>{0, 1}).equals<i64>({30, 30}));
}

TEST_CASE("sample copies set removals and does not publish unchanged membership",
          "[feature:operator][feature:collection-change][series:tss][activity:passive]"
          "[transition:removal]") {
    const auto set = node<changing_set_source>();
    const auto inspect = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size() * 10 + input.contains(2)));
    });
    const auto graph = [&](wp<TS<i64>> signal) { return inspect(operators::sample(signal, set())); };

    CHECK(testing::test_graph(graph, std::array<i64, 4>{0, 1, 2, 3})
              .equals<i64>({0, 21, 10, std::nullopt}));
}

TEST_CASE("dedup suppresses consecutive equal values",
          "[feature:operator][feature:type-system][series:value][guarantee:deduplicated]") {
    const auto deduplicate = [](wp<TS<i64>> value) { return operators::dedup(value); };

    CHECK(testing::test_graph(deduplicate, std::array<i64, 4>{1, 1, 2, 2})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("dedup publishes the same value after invalidation",
          "[feature:operator][feature:validity][series:value][guarantee:deduplicated]"
          "[transition:invalidation][transition:revalidation]") {
    const auto invalidate = node([](In<TS<i64>> value, Out<TS<i64>> output) {
        if (*value < 0) {
            output.invalidate();
            return;
        }
        output.copy(value);
    });
    const auto graph = [&](wp<TS<i64>> value) { return operators::dedup(invalidate(value)); };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{1, -1, 1}).equals<i64>({1, std::nullopt, 1}));
}

TEST_CASE("dedup leaves an already deduplicated input as the same graph node",
          "[feature:operator][feature:type-system][series:value][guarantee:deduplicated]"
          "[shape:chain]") {
    const auto source = node<deduplicated_source>();
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto graph = [&] { return copy(operators::dedup(source())); };
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        graph();
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    CHECK(compiled.node_count() == 2);
    CHECK(testing::test_graph(graph).equals<i64>({1}));
}

TEST_CASE("dedup on a reference observes retargeted values",
          "[feature:operator][feature:reference][feature:type-system][series:reference]"
          "[guarantee:deduplicated][binding:reference-to-value][transition:update]") {
    const auto first = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.set(1); });
    const auto second = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.set(2); });
    const auto select = node([](In<TS<i64>> command, In<REF<TS<i64, TimeSeriesGuarantee::deduplicated>>, passive> first,
                                In<REF<TS<i64, TimeSeriesGuarantee::deduplicated>>, passive> second,
                                Out<REF<TS<i64, TimeSeriesGuarantee::deduplicated>>> output) {
        output.copy(*command == 0 ? first : second);
    });
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto graph = [&](wp<TS<i64>> command) { return copy(operators::dedup(select(command, first(), second()))); };

    CHECK(testing::test_graph(graph, std::array<i64, 4>{0, 0, 1, 1})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("dedup rejects scalar types without equality",
          "[feature:operator][feature:type-system][series:value][scalar:opaque]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    using VALUE = Opaque<stream_test::non_comparable_value>;
    (void)cpp_scalar_runtime_type<VALUE>();
    const auto source = node(
        eval = [](Out<OPAQUE_SERIES>) {},
        resolve<OPAQUE_SERIES> = []() -> const TimeSeriesType & {
            return time_series_value_type(cpp_scalar_type<VALUE>(), TimeSeriesGuarantee::ordinary);
        });
    auto wiring = GraphWiring{};

    try {
        auto context = WiringContext{wiring};
        static_cast<void>(operators::dedup(source()));
        FAIL("expected dedup to reject a scalar type without equality");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(std::ranges::any_of(error.diagnostic()->candidates, [](const auto &candidate) {
            return candidate.message.find("requires a scalar type that supports equality") != std::string::npos;
        }));
    }
}

TEST_CASE("dedup returns final and const opaque inputs without an equality node",
          "[feature:operator][feature:type-system][series:value][scalar:opaque]"
          "[guarantee:final][guarantee:const][shape:single-node]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    using VALUE = Opaque<stream_test::non_comparable_value>;
    (void)cpp_scalar_runtime_type<VALUE>();

    const auto make_source = [](TimeSeriesGuarantee guarantee) {
        return node(
            eval = [](Out<OPAQUE_SERIES>) {},
            resolve<OPAQUE_SERIES> = [guarantee]() -> const TimeSeriesType & {
                return time_series_value_type(cpp_scalar_type<VALUE>(), guarantee);
            });
    };
    const auto check_unchanged = [&](TimeSeriesGuarantee guarantee) {
        const auto source = make_source(guarantee);
        auto wiring = GraphWiring{};
        const TimeSeriesType *result_type;
        {
            auto context = WiringContext{wiring};
            result_type = &operators::dedup(source()).type();
        }
        wiring.finalize();

        CHECK(result_type == &time_series_value_type(cpp_scalar_type<VALUE>(), guarantee));
        CHECK(wiring.nodes().size() == 1);
    };

    check_unchanged(TimeSeriesGuarantee::final);
    check_unchanged(TimeSeriesGuarantee::const_);
}
