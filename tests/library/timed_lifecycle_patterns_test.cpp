#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/nested.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;

TEST_CASE("map removes pending delayed output with its key",
          "[feature:map][feature:timed-scheduling][feature:nested-graph]"
          "[series:tsd][transition:removal][transition:update]") {
    const auto delay_value = [](wp<TS<i64>> value) {
        return operators::delay(value, timedelta_t{2});
    };

    const auto materialize = node([](In<TSD<i64, TS<i64>>> values,
                                     Out<TSD<i64, TS<i64>>> output) { output.copy_tick(values); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return materialize(map(delay_value, values));
    };

    const auto actual = run_graph(tested,
                    {{{1, 10}, {2, 100}},
                     {{1, literal::REMOVE}, {2, 101}},
                     {{1, 11}},
                     literal::SKIP,
                     literal::SKIP});
    CHECK(actual.equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                         {{1, literal::REMOVE}},
                         {{1, literal::INVALID}, {2, 100}},
                         {{2, 101}},
                         {{1, 11}}}));
}

TEST_CASE("map keeps throttling state independent across key removal and recreation",
          "[feature:map][feature:timed-scheduling][feature:nested-graph][feature:lifecycle]"
          "[series:tsd][transition:removal][transition:addition]") {
    const auto throttle_value = [](wp<TS<i64>> value) {
        return operators::throttle(value, timedelta_t{3});
    };

    const auto materialize = node([](In<TSD<i64, TS<i64>>> values,
                                     Out<TSD<i64, TS<i64>>> output) { output.copy_tick(values); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return materialize(map(throttle_value, values));
    };

    const auto actual = run_graph(tested,
                    {{{1, 10}, {2, 100}},
                     {{1, literal::REMOVE}, {2, 101}},
                     {{1, 11}},
                     {{2, 102}},
                     literal::SKIP});
    CHECK(actual.equals({{{1, 10}, {2, 100}},
                       {{1, literal::REMOVE}},
                       {{1, 11}},
                       {{2, 102}},
                       literal::SKIP}));
}

TEST_CASE("mapped delays cancel nested collection deltas when an outer key is removed",
          "[feature:map][feature:timed-scheduling][feature:nested-graph]"
          "[series:tsd][shape:nested][transition:removal][transition:update]") {
    const auto delay_collection = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::delay(values, timedelta_t{3});
    };

    const auto materialize = node([](In<TSD<i64, TSD<i64, TS<i64>>>> values,
                                     Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return materialize(map(delay_collection, values));
    };

    const auto actual = run_graph(tested,
                    {{{1, {{10, 1}}}, {2, {{20, 2}}}},
                     {{1, {{10, literal::REMOVE}}}, {2, {{20, 3}}}},
                     {{1, literal::REMOVE}, {2, {{21, 4}}}},
                     {{1, {{11, 5}}}},
                     literal::SKIP,
                     literal::SKIP});
    CHECK(actual.equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                         literal::SKIP,
                         {{1, literal::REMOVE}},
                         {{1, literal::INVALID}, {2, {{20, 2}}}},
                         {{2, {{20, 3}}}},
                         {{2, {{21, 4}}}},
                         {{1, {{11, 5}}}}}));
}

TEST_CASE("a restarted nested delay publishes only values from its new lifetime",
          "[feature:nested-graph][feature:timed-scheduling][feature:lifecycle]"
          "[series:value][transition:stop][transition:restart]") {
    const auto delayed = graph("name"_n = "lifecycle_delayed_value", [](wp<TS<i64>> value) {
        return operators::delay(value, timedelta_t{2});
    });

    const auto tested = [&](wp<TS<bool>> run, wp<TS<i64>> value) {
        return nested(delayed, run, value);
    };

    CHECK(run_graph(tested, {true, false, true, true}, {10, 20, 30, 40})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, std::nullopt, 30, 40}));
}

TEST_CASE("switching one mapped key cancels only that branch's pending delay",
          "[feature:map][feature:switch][feature:timed-scheduling][feature:nested-graph]"
          "[series:tsd][series:value][shape:nested][transition:update][transition:stop]") {
    const auto delay_value = [](wp<TS<i64>> value) {
        return operators::delay(value, timedelta_t{4});
    };
    const auto identity = [](wp<TS<i64>> value) { return value; };
    const auto select_delay = [&](wp<TS<i64>> value, wp<TS<bool>> use_delay) {
        return switch_(use_delay,
                       cases{case_(true, delay_value), case_(false, identity)},
                       value);
    };

    const auto materialize = node([](In<TSD<i64, TS<i64>>> values,
                                     Out<TSD<i64, TS<i64>>> output) { output.copy_tick(values); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TSD<i64, TS<bool>>> use_delay) {
        return materialize(map(select_delay, values, use_delay));
    };

    const auto actual = run_graph(tested,
                    {{{1, 10}, {2, 20}},
                     {{1, 11}, {2, 21}},
                     {{1, 12}, {2, 22}},
                     {{1, 13}, {2, 23}},
                     literal::SKIP},
                    {{{1, true}, {2, true}},
                     {{1, false}},
                     {{1, true}},
                     {{2, false}},
                     literal::SKIP});
    CHECK(actual.equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                         {{1, 11}},
                         {{1, literal::INVALID}},
                         {{2, 23}},
                         literal::SKIP,
                         literal::SKIP,
                         {{1, 12}},
                         {{1, 13}}}));
}
