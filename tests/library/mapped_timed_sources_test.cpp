#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;

TEST_CASE("mapped schedules keep independent start times",
          "[feature:map][feature:timed-scheduling][feature:nested-graph]"
          "[series:tss][series:tsd][transition:addition]") {
    const auto schedule = graph([] {
        return operators::schedule(timedelta_t{2}, true, i64{2});
    });

    const auto copy = node([](In<TSD<i64, TS<bool>>> input, Out<TSD<i64, TS<bool>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSS<i64>> keys) {
        return copy(map(schedule, keys));
    };

    const auto actual = run_graph(tested, {{1}, {2}});
    CHECK(actual.equals({{{1, literal::INVALID}},
                         {{2, literal::INVALID}},
                         {{1, true}},
                         {{2, true}},
                         {{1, true}},
                         {{2, true}}}));
}

TEST_CASE("mapped immediate schedules publish during branch startup",
          "[feature:map][feature:timed-scheduling][feature:nested-graph]"
          "[series:tss][series:tsd][transition:addition][transition:start]") {
    const auto schedule = graph([] {
        return operators::schedule(timedelta_t{2}, false, i64{2});
    });

    const auto copy = node([](In<TSD<i64, TS<bool>>> input, Out<TSD<i64, TS<bool>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSS<i64>> keys) { return copy(map(schedule, keys)); };

    const auto actual = run_graph(tested, {{1}, {2}});
    CHECK(actual.equals({{{1, true}},
                         {{2, true}},
                         {{1, true}},
                         {{2, true}}}));
}

TEST_CASE("mapped schedule removal cancels its timer and recreation starts a new schedule",
          "[feature:map][feature:timed-scheduling][feature:nested-graph][feature:lifecycle]"
          "[series:tss][series:tsd][transition:removal][transition:addition][transition:restart]") {
    const auto schedule = graph([] {
        return operators::schedule(timedelta_t{2}, true, i64{2});
    });

    const auto copy = node([](In<TSD<i64, TS<bool>>> input, Out<TSD<i64, TS<bool>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSS<i64>> keys) {
        return copy(map(schedule, keys));
    };

    const auto actual = run_graph(tested, {{1}, {2}, {literal::REMOVED(1)}, {1}});
    CHECK(actual.equals({{{1, literal::INVALID}},
                         {{2, literal::INVALID}},
                         {{1, literal::REMOVE}},
                         {{1, literal::INVALID}, {2, true}},
                         literal::SKIP,
                         {{1, true}, {2, true}},
                         literal::SKIP,
                         {{1, true}}}));
}

TEST_CASE("mapped resampling keeps per-key clocks",
          "[feature:map][feature:timed-scheduling][feature:nested-graph]"
          "[series:tsd][transition:addition][transition:update]") {
    const auto resample = [](wp<TS<i64>> value) {
        return operators::resample(value, timedelta_t{2});
    };

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return copy(map(resample, values));
    };

    const auto actual = run_graph(tested,
                                  GraphRunOptions{.run_time = EvaluationTime::duration{7}},
                                  {{{1, 10}}, {{2, 20}}, {{1, 11}}, {{2, 21}}, {{1, 12}}, {{2, 22}}});
    CHECK(actual.equals({{{1, literal::INVALID}},
                         {{2, literal::INVALID}},
                         {{1, 11}},
                         {{2, 21}},
                         {{1, 12}},
                         {{2, 22}},
                         {{1, 12}}}));
}

TEST_CASE("mapped resampling observes an input update on its timer",
          "[feature:map][feature:timed-scheduling][feature:nested-graph]"
          "[series:tsd][transition:update]") {
    const auto resample = [](wp<TS<i64>> value) {
        return operators::resample(value, timedelta_t{2});
    };

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy(map(resample, values)); };

    const auto actual = run_graph(tested,
                                  GraphRunOptions{.run_time = EvaluationTime::duration{5}},
                                  {{{1, 10}}, {{1, 11}}, {{1, 12}}, {{1, 13}}});
    CHECK(actual.equals({{{1, literal::INVALID}},
                         literal::SKIP,
                         {{1, 12}},
                         literal::SKIP,
                         {{1, 13}}}));
}

TEST_CASE("mapped resampling removes a due branch and restarts it beside a live branch",
          "[feature:map][feature:timed-scheduling][feature:nested-graph][feature:lifecycle]"
          "[series:tsd][transition:removal][transition:addition][transition:restart][transition:update]") {
    const auto resample = [](wp<TS<i64>> value) {
        return operators::resample(value, timedelta_t{2});
    };

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return copy(map(resample, values)); };

    const auto actual = run_graph(tested,
                                  GraphRunOptions{.run_time = EvaluationTime::duration{7}},
                                  {{{1, 10}, {2, 20}},
                                   literal::SKIP,
                                   {{1, literal::REMOVE}, {2, 21}},
                                   {{1, 11}, {2, 22}}});
    CHECK(actual.equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                         literal::SKIP,
                         {{1, literal::REMOVE}, {2, 21}},
                         {{1, literal::INVALID}},
                         {{2, 22}},
                         {{1, 11}},
                         {{2, 22}}}));
}
