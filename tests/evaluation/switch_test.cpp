#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

namespace {

using namespace bg;
using namespace bg::literals;

const auto add = node([](In<TS<i64>> value, int amount, Out<TS<i64>> output) {
    output.set(*value + amount);
});

struct SelectedArgumentGraph {
    wp<TS<i64>> selected;

    auto wire() const { return add(selected, 1); }
};

struct StatefulCase {
    In<TS<i64>> value;
    State<int> instance;
    Out<TS<i64>> output;

    void start() const { *instance = ++start_count; }
    void evaluate() const { output.set(*instance * 100 + *value); }

    inline static int start_count{};
};

const auto stateful_case = node<StatefulCase>();

struct RecordingSink {
    In<TS<i64>> value;

    void evaluate() const {
        ++count;
        last_value = *value;
    }

    inline static int count{};
    inline static int last_value{};
};

const auto recording_sink = node<RecordingSink>();

const auto optional_key = node([](In<TS<std::int64_t>> value,
                                  Out<TS<std::int64_t>> output) {
    if (*value < 0) {
        output.invalidate();
    } else {
        output.set(*value);
    }
});

struct DelayedCase {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const {
        ++evaluation_count;
        output.set(17);
    }

    inline static int evaluation_count{};
};

const auto delayed_case = node<DelayedCase>();

struct MappedSwitchCase {
    In<TS<i64>> trigger;
    int offset;
    State<int> evaluation_count;
    Out<TS<i64>> output;

    void start() const { *evaluation_count = 0; }

    void evaluate() const { output.set(offset + ++*evaluation_count); }
};

const auto mapped_switch_case = node<MappedSwitchCase>();

const auto mapped_values = node([](In<TS<i64>> trigger,
                                   Out<TSD<std::int64_t, TS<i64>>> output) {
    output[1].set(*trigger);
    output[2].set(*trigger);
});

const auto mapped_selectors = node([](In<TS<std::int64_t>> first,
                                      In<TS<std::int64_t>> second,
                                      Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
    output[1].set(*first);
    output[2].set(*second);
});

const auto optional_mapped_values = node([](In<TS<i64>> trigger, In<TS<bool>> present,
                                            Out<TSD<std::int64_t, TS<i64>>> output) {
    if (*present) {
        output[1].set(*trigger);
    } else {
        output.erase(1);
    }
});

const auto optional_mapped_selectors = node(
    [](In<TS<std::int64_t>> selector, In<TS<bool>> present,
       Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        if (*present) {
            output[1].set(*selector);
        } else {
            output.erase(1);
        }
    });

const auto selectors_at_key_one = node(
    [](In<TS<std::int64_t>> selector,
       Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        output[1].set(*selector);
    });

const auto add_mapped_results = node([](In<TS<i64>, optional> first,
                                        In<TS<i64>, optional> second,
                                        Out<TS<i64>> output) {
    if (first.valid() && second.valid()) {
        output.set(*first + *second);
    } else {
        output.invalidate();
    }
});

const auto mapped_switch = graph(
    "name"_n = "mapped_switch",
    [](wp<TS<i64>> trigger, wp<TS<std::int64_t>> selector) {
        return switch_(
                   selector,
                   cases{
                       case_(1, [](auto value) { return mapped_switch_case(value, 10); }),
                       case_(2, [](auto value) { return mapped_switch_case(value, 20); }),
                   },
                   trigger)
            .as<TS<i64>>();
    });

const auto delayed_mapped_switch = graph(
    "name"_n = "delayed_mapped_switch",
    [](wp<TS<std::int64_t>> selector) {
        return switch_(
                   selector,
                   cases{case_(1, []() -> wp<TS<i64>> { return delayed_case(); })})
            .as<TS<i64>>();
    });

TEST_CASE("switch selects a case graph from its key",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][definition:function]") {
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(std::int64_t{1}, [](auto input) { return add(input, 10); }),
                       case_(std::int64_t{2}, [](auto input) { return add(input, 20); }),
                   },
                   value)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 4>{1, 2, 2, 1},
                                            std::array{3, 3, 4, 5});

    CHECK(actual.equals<bg::i64>({13, 23, 24, 15}));
}

TEST_CASE("switch uses DEFAULT for invalid and unmatched keys",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:invalidation][definition:function]") {
    const auto selected = graph("name"_n = "selected", [](wp<TS<i64>> value) {
        return add(value, 10);
    });
    const auto fallback = graph("name"_n = "fallback", [](wp<TS<i64>> value) {
        return add(value, 100);
    });
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(key,
                       cases{case_(std::int64_t{1}, selected), case_(DEFAULT, fallback)},
                       value)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 9},
                                            std::array{2, 3});

    CHECK(actual.equals<bg::i64>({12, 103}));
}

TEST_CASE("switch reports an unmatched or invalid key without DEFAULT",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][feature:validity][transition:invalidation][definition:function]") {
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(key,
                       cases{case_(std::int64_t{1}, [](auto input) { return add(input, 1); })},
                       value)
            .as<TS<i64>>();
    };

    CHECK_THROWS_WITH(
        (void)testing::test_graph(tested, std::array<std::int64_t, 2>{1, 9},
                                  std::array{5, 6}),
        "switch_ key does not match a case and no DEFAULT case is defined");

    const auto invalid_tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(optional_key(key),
                       cases{case_(std::int64_t{1}, [](auto input) { return add(input, 1); })},
                       value)
            .as<TS<i64>>();
    };
    CHECK_THROWS_WITH(
        (void)testing::test_graph(invalid_tested, std::array<std::int64_t, 1>{-1},
                                  std::array{5}),
        "switch_ key does not match a case and no DEFAULT case is defined");
}

TEST_CASE("an outputless DEFAULT explicitly invalidates a switch output",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[shape:sink][series:value][transition:invalidation][definition:function]") {
    RecordingSink::count = 0;
    RecordingSink::last_value = 0;
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(DEFAULT, [](auto input) { recording_sink(input); }),
                       case_(1, [](auto input) { return add(input, 10); }),
                   },
                   value)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 9},
                                            std::array{2, 3});

    CHECK(actual.equals<bg::i64>({12, std::nullopt}));
    CHECK(RecordingSink::count == 1);
    CHECK(RecordingSink::last_value == 3);
}

TEST_CASE("switch matches named graph arguments and omits unused named arguments",
          "[feature:switch][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[shape:nested][series:value][definition:aggregate]") {
    const auto selected = graph<SelectedArgumentGraph>();
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<i64>> value,
                            wp<TS<i64>> unused) {
        return switch_(key, cases{case_(std::int64_t{1}, selected)},
                       "selected"_n = value, "unused"_n = unused)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 2>{1, 1}, std::array{3, 5}, std::array{40, 60});

    CHECK(actual.equals<bg::i64>({4, 6}));
}

TEST_CASE("switch forwards positional arguments to a variadic case graph",
          "[feature:switch][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[shape:nested][series:value][series:tsb][schema:generic][definition:function]") {
    const auto select_second = graph(
        "name"_n = "select_second_switch_argument",
        [](wp<TSB<SCHEMA>, var_args> arguments) {
            return wp<TS<i64>>{arguments[1].time_series()};
        });
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<i64>> first,
                            wp<TS<i64>> second) {
        return switch_(key, cases{case_(1, select_second)}, first, second).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 2>{1, 1}, std::array{3, 5}, std::array{40, 60});

    CHECK(actual.equals<bg::i64>({40, 60}));
}

TEST_CASE("switch forwards named arguments to a variadic case graph",
          "[feature:switch][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[shape:nested][series:value][series:tsb][schema:generic][definition:function]") {
    const auto select_item = graph(
        "name"_n = "select_named_switch_argument",
        [](wp<TSB<SCHEMA>, var_kwargs> arguments) {
            return wp<TS<i64>>{arguments["item"].time_series()};
        });
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(key, cases{case_(1, select_item)}, "item"_n = value).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 2>{1, 1}, std::array{3, 5});

    CHECK(actual.equals<bg::i64>({3, 5}));
}

TEST_CASE("switch captures scalar case arguments at wiring time",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][definition:function]") {
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{case_(1, [](auto input, int amount) { return add(input, amount); })},
                   value, 10)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 2>{1, 1}, std::array{3, 5});

    CHECK(actual.equals<bg::i64>({13, 15}));
}

TEST_CASE("equal switch key ticks retain inner graph state and returning recreates it",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:update][transition:removal][definition:function]") {
    StatefulCase::start_count = 0;
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(std::int64_t{1}, [](auto input) { return stateful_case(input); }),
                       case_(std::int64_t{2}, [](auto input) { return stateful_case(input); }),
                   },
                   value)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 4>{1, 1, 2, 1},
                                            std::array{1, 2, 3, 4});

    CHECK(actual.equals<bg::i64>({101, 102, 203, 304}));
    CHECK(StatefulCase::start_count == 3);
}

TEST_CASE("switching between tuple keys recreates the case graph",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:update][transition:removal][definition:function]") {
    StatefulCase::start_count = 0;
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(std::tuple{1, 2},
                             [](auto input) { return stateful_case(input); }),
                   },
                   value)
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 4>{1, 1, 2, 2},
                                            std::array{1, 2, 3, 4});

    CHECK(actual.equals<bg::i64>({101, 102, 203, 204}));
    CHECK(StatefulCase::start_count == 2);
}

TEST_CASE("switch supports source and sink case graphs",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[shape:source][shape:sink][series:value][definition:function]") {
    const auto source_tested = [](wp<TS<std::int64_t>> key) {
        return switch_(key,
                       cases{case_(std::int64_t{1}, []() -> wp<TS<std::int64_t,
                                                                  TimeSeriesGuarantee::const_>> {
                           return operators::const_(42).as<TS<std::int64_t,
                                                               TimeSeriesGuarantee::const_>>();
                       })})
            .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };
    CHECK(testing::test_graph(source_tested, std::array<std::int64_t, 1>{1})
              .equals<std::int64_t>({42}));

    RecordingSink::count = 0;
    RecordingSink::last_value = 0;
    const auto sink_tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        (void)switch_(key,
                      cases{case_(std::int64_t{1}, [](auto input) { recording_sink(input); })},
                      value);
        return value;
    };
    CHECK(testing::test_graph(sink_tested, std::array<std::int64_t, 2>{1, 1},
                              std::array{7, 9})
              .equals<bg::i64>({7, 9}));
    CHECK(RecordingSink::count == 2);
    CHECK(RecordingSink::last_value == 9);
}

TEST_CASE("switch accepts fully runtime cases and arguments",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][definition:runtime]") {
    const auto branch = graph("name"_n = "runtime_switch_branch", [](wp<TS<i64>> value) {
        return add(value, 3);
    });
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        const auto runtime_keys = std::array{
            WiringArgument{std::int64_t{1}}, WiringArgument{std::int64_t{2}}};
        const auto runtime_cases = std::array{
            SwitchCaseArgument{std::span<const WiringArgument>{runtime_keys}, branch},
        };
        const auto runtime_arguments = std::array{WiringArgument{value}};
        return switch_(static_cast<WiringPoint>(key),
                       std::span<const SwitchCaseArgument>{runtime_cases},
                       std::span<const WiringArgument>{runtime_arguments})
            .as<TS<i64>>();
    };

    CHECK(testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2}, std::array{2, 5})
              .equals<bg::i64>({5, 8}));
}

TEST_CASE("switch DEFAULT handles an invalid key",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:invalidation][definition:function]") {
    const auto tested = [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
        return switch_(optional_key(key),
                       cases{
                           case_(std::int64_t{1}, [](auto input) { return add(input, 10); }),
                           case_(DEFAULT, [](auto input) { return add(input, 100); }),
                       },
                       value)
            .as<TS<i64>>();
    };

    CHECK(testing::test_graph(tested, std::array<std::int64_t, 2>{1, -1}, std::array{2, 3})
              .equals<bg::i64>({12, 103}));
}

TEST_CASE("switch starts a DEFAULT source when its key is initially invalid",
          "[feature:switch][feature:nested-graph][feature:operator][shape:source]"
          "[series:value][transition:invalidation][definition:function]") {
    const auto tested = [](wp<TS<std::int64_t>> key) {
        return switch_(
                   optional_key(key),
                   cases{case_(DEFAULT, [] {
                       return operators::const_(7).as<TS<std::int64_t,
                                                          TimeSeriesGuarantee::const_>>();
                   })})
            .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };

    CHECK(testing::test_graph(tested, std::array<std::int64_t, 1>{-1})
              .equals<std::int64_t>({7}));
}

TEST_CASE("a switch inner graph reports timed work through its owner",
          "[feature:switch][feature:nested-graph][feature:operator][feature:timed-scheduling]"
          "[shape:nested][shape:source][series:value][definition:function]") {
    DelayedCase::evaluation_count = 0;
    const auto branch = graph("name"_n = "delayed_switch_case",
                              []() -> wp<TS<i64>> { return delayed_case(); });

    run_graph([&] {
        const auto key = operators::const_(std::int64_t{1})
                             .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
        (void)switch_(key, cases{case_(std::int64_t{1}, branch)});
    });

    CHECK(DelayedCase::evaluation_count == 1);
}

TEST_CASE("switches inside a map retain and replace state independently",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[shape:doubly-nested][series:value][series:tsd][transition:update]"
          "[definition:function]") {
    const auto tested = [](wp<TS<i64>> trigger, wp<TS<std::int64_t>> first,
                           wp<TS<std::int64_t>> second) {
        const auto result = map(mapped_switch, mapped_values(trigger),
                                mapped_selectors(first, second));
        return add_mapped_results(result[std::int64_t{1}].as<TS<i64>>(),
                                  result[std::int64_t{2}].as<TS<i64>>());
    };

    const auto actual = testing::test_graph(
        tested, std::array{0, 0, 0, 0}, std::array<std::int64_t, 4>{1, 1, 2, 2},
        std::array<std::int64_t, 4>{2, 2, 2, 1});

    CHECK(actual.equals<bg::i64>({32, 34, 44, 33}));
}

TEST_CASE("removing and recreating a map entry recreates its switch graph",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[shape:doubly-nested][series:value][series:tsd][transition:addition]"
          "[transition:removal][definition:function]") {
    const auto tested = [](wp<TS<i64>> trigger, wp<TS<std::int64_t>> selector,
                           wp<TS<bool>> present) {
        const auto result = map(mapped_switch, optional_mapped_values(trigger, present),
                                optional_mapped_selectors(selector, present));
        return result[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array{0, 0, 0, 0}, std::array<std::int64_t, 4>{1, 1, 1, 1},
        std::array{true, true, false, true});

    CHECK(actual.equals<bg::i64>({11, 12, std::nullopt, 11}));
}

TEST_CASE("an unmatched switch key inside a map fails the outer graph",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[feature:lifecycle][shape:doubly-nested][series:value][series:tsd]"
          "[definition:function]") {
    const auto tested = [](wp<TS<i64>> trigger, wp<TS<std::int64_t>> selector,
                           wp<TS<bool>> present) {
        const auto result = map(mapped_switch, optional_mapped_values(trigger, present),
                                optional_mapped_selectors(selector, present));
        return result[std::int64_t{1}].as<TS<i64>>();
    };

    CHECK_THROWS_WITH(
        (void)testing::test_graph(tested, std::array{0},
                                  std::array<std::int64_t, 1>{9}, std::array{true}),
        "switch_ key does not match a case and no DEFAULT case is defined");
}

TEST_CASE("timed scheduling propagates through a switch inside a map",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[feature:timed-scheduling][shape:doubly-nested][shape:source]"
          "[series:value][series:tsd][definition:function]") {
    DelayedCase::evaluation_count = 0;

    run_graph([] {
        const auto selector = operators::const_(std::int64_t{1})
                                  .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
        (void)map(delayed_mapped_switch, selectors_at_key_one(selector));
    });

    CHECK(DelayedCase::evaluation_count == 1);
}

TEST_CASE("switch crosses an additional nested graph boundary",
          "[feature:switch][feature:nested-graph][feature:operator][shape:doubly-nested]"
          "[series:value][binding:value-to-reference][binding:reference-to-value]"
          "[definition:function]") {
    const auto inner = graph(
        "name"_n = "nested_switch",
        [](wp<TS<std::int64_t>> key, wp<TS<i64>> value) {
            return switch_(key,
                           cases{case_(1, [](auto input) { return add(input, 10); }),
                                 case_(2, [](auto input) { return add(input, 20); })},
                           value)
                .as<TS<i64>>();
        });
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<i64>> value,
                            wp<TS<bool>> run) {
        return nested(inner, run, key, value);
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 4>{1, 2, 2, 1}, std::array{3, 3, 4, 5},
        std::array{true, true, true, true});

    CHECK(actual.equals<bg::i64>({13, 23, 24, 15}));
}

} // namespace
