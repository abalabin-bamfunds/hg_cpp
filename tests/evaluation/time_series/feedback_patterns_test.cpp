#include <hg/feedback.hpp>
#include <hg/graph.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsl/tsl.hpp>
#include <hg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace hg;
using namespace hg::literals;

TEST_CASE("a feedback accumulator can use an optional prior value as its initial state",
          "[feature:feedback][series:value][scalar:i64][activity:passive][transition:first-tick]"
          "[transition:update]") {
    const auto add = node([](i64 delta, In<TS<i64>, optional, passive> previous) {
        return delta + (previous.valid() ? *previous : 0);
    });

    const auto accumulate = [&](wp<TS<i64>> deltas) {
        auto loop = feedback<TS<i64>>();
        loop = add(deltas, loop);
        wp<TS<i64>> output = loop;
        return output;
    };

    CHECK(run_graph(accumulate, {2, 3, 4}).equals({std::nullopt, 2, 5, 9}));
}

namespace {

template <template <typename, auto...> typename Field> struct FeedbackRecord {
    Field<TS<i64>> count;
    Field<TS<i64>> value;
};

} // namespace

TEST_CASE("feedback delays each field change in a bundle",
          "[feature:feedback][feature:time-series-storage][series:tsb][schema:named]"
          "[transition:first-tick][transition:update]") {
    const auto delayed = [](wp<TSB<FeedbackRecord>> input) {
        auto loop = feedback<TSB<FeedbackRecord>>();
        loop = input;
        wp<TSB<FeedbackRecord>> output = loop;
        return output;
    };

    CHECK(run_graph(delayed,
                    {{{"count", 1}, {"value", 10}}, {{"value", 11}}, {{"count", 2}}})
              .equals({literal::SKIP,
                       {{"count", 1}, {"value", 10}},
                       {{"value", 11}},
                       {{"count", 2}}}));
}

TEST_CASE("feedback delays membership changes in a set",
          "[feature:feedback][feature:time-series-storage][series:tss][scalar:i64]"
          "[transition:addition][transition:removal]") {
    const auto delayed = [](wp<TSS<i64>> input) {
        auto loop = feedback<TSS<i64>>();
        loop = input;
        wp<TSS<i64>> output = loop;
        return output;
    };

    CHECK(run_graph(delayed, {{1, 2}, {3}, {literal::REMOVED(1)}})
              .equals({literal::SKIP, {1, 2}, {3}, {literal::REMOVED(1)}}));
}

TEST_CASE("feedback preserves sparse changes in a fixed list",
          "[feature:feedback][feature:time-series-storage][series:fixed-tsl][transition:update]") {
    const auto delayed = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        auto loop = feedback<TSL<TS<i64>, 2>>();
        loop = tsl(first, second);
        wp<TSL<TS<i64>, 2>> output = loop;
        return output;
    };

    CHECK(run_graph(delayed, {1, literal::SKIP}, {literal::SKIP, 10})
              .equals({literal::SKIP, {{0, 1}}, {{1, 10}}}));
}

TEST_CASE("feedback retains separate state for each mapped key",
          "[feature:feedback][feature:map][feature:nested-graph][series:tsd][series:value]"
          "[shape:nested][transition:addition][transition:update][transition:removal]") {
    const auto delay = graph("name"_n = "delay_each_item", [](wp<TS<i64>> value) {
        auto loop = feedback<TS<i64>>();
        loop = value;
        wp<TS<i64>> output = loop;
        return output;
    });

    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) { return materialize(map(delay, values)); };

    const auto actual = run_graph(tested,
                                  {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}, {{2, 30}}});
    CHECK(actual.equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                         {{1, 10}, {2, 20}},
                         {{1, 11}, {2, literal::REMOVE}},
                         {{2, literal::INVALID}},
                         {{2, 30}}}));
}

TEST_CASE("mapped feedback accumulators keep state per key",
          "[feature:feedback][feature:map][feature:nested-graph][series:tsd][series:value]"
          "[shape:nested][transition:update]") {
    const auto add = node([](i64 delta, In<TS<i64>, optional, passive> previous) {
        return delta + (previous.valid() ? *previous : 0);
    });

    const auto accumulate = graph("name"_n = "accumulate_each_item", [&](wp<TS<i64>> delta) {
        auto loop = feedback<TS<i64>>();
        loop = add(delta, loop);
        wp<TS<i64>> output = loop;
        return output;
    });

    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> deltas) { return materialize(map(accumulate, deltas)); };

    CHECK(run_graph(tested, {{{1, 2}, {2, 10}}, {{1, 3}}, {{2, 1}}})
              .equals({{{1, literal::INVALID}, {2, literal::INVALID}},
                       {{1, 2}, {2, 10}},
                       {{1, 5}},
                       {{2, 11}}}));
}

TEST_CASE("passive feedback accumulation stops when the input stops",
          "[feature:feedback][feature:input-activity][series:value][scalar:i64]"
          "[activity:passive][transition:update]") {
    const auto accumulate = node([](i64 delta, In<TS<i64>, optional, passive> previous) {
        return delta + (previous.valid() ? *previous : 0);
    });

    const auto graph = [&](wp<TS<i64>> deltas) {
        auto loop = feedback<TS<i64>>();
        loop = accumulate(deltas, loop);
        wp<TS<i64>> output = loop;
        return output;
    };

    CHECK(run_graph(graph, {1, 2, literal::SKIP, 3}).equals({std::nullopt, 1, 3, std::nullopt, 6}));
}
