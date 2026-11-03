#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/nested.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace hg;
using namespace hg::literals;

TEST_CASE("convert TSS snapshots to scalar sets and sum their values",
          "[feature:operator][feature:scalar-collection][series:tss]"
          "[scalar:set][transition:addition][transition:removal]") {
    const auto total = [](wp<TSS<i64>> values) {
        const auto snapshot = operators::convert[type<TS<Set<i64>>>](values);
        return operators::sum_(snapshot);
    };

    CHECK(run_graph(total, {{1, 2}, {literal::REMOVED(1), 3},
                            {literal::REMOVED(2), literal::REMOVED(3)}})
              .equals<i64>({3, 5, 0}));
}

TEST_CASE("convert fixed TSL snapshots to scalar lists and sum their values",
          "[feature:operator][series:fixed-tsl][scalar:list][scalar:i64]"
          "[transition:update]") {
    const auto total = [](wp<TSL<TS<i64>, 2>> values) {
        const auto snapshot = operators::convert[type<TS<List<i64>>>](values);
        return operators::sum_(snapshot);
    };

    CHECK(run_graph(total, {{{0, 1}, {1, 2}}, {{0, 5}}}).equals<i64>({3, 7}));
}

TEST_CASE("collect resettable scalar sets, convert them to TSS, and sum membership",
          "[feature:operator][feature:collection-change][series:value][series:tss]"
          "[scalar:set][transition:update]") {
    const auto total = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        const auto snapshot = operators::collect[type<TS<Set<i64>>>](value, "reset"_n = reset);
        const auto members = operators::convert[type<TSS<i64>>](snapshot);
        return operators::sum_("values"_n = members);
    };

    CHECK(run_graph(total, {1, 1, 2, 2}, {literal::SKIP, literal::SKIP, false, literal::SKIP})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("collect keyed events, convert the snapshot to TSD, and sum its values",
          "[feature:operator][feature:collection-change][series:tsd]"
          "[scalar:dict][transition:update]") {
    const auto total = [](wp<TS<i64>> key, wp<TS<i64>> value, wp<TS<bool>> reset) {
        const auto snapshot = operators::collect[type<TS<Dict<i64, i64>>>](
            "key"_n = key, "value"_n = value, "reset"_n = reset);
        const auto values = operators::convert[type<TSD<i64, TS<i64>>>](snapshot);
        return operators::sum_("values"_n = values);
    };

    CHECK(run_graph(total, {1, 2, 1, 2}, {10, 20, 12, 25},
                    {literal::SKIP, literal::SKIP, false, literal::SKIP})
              .equals<i64>({10, 30, 12, 37}));
}

TEST_CASE("convert a changing TSD to scalar dictionaries and emit key/value entries",
          "[feature:operator][feature:timed-scheduling][series:tsd][scalar:dict]"
          "[transition:update][transition:removal][transition:addition]") {
    const auto emit_entries = [](wp<TSD<i64, TS<i64>>> values) {
        const auto snapshot = operators::convert[type<TS<Dict<i64, i64>>>](values);
        return operators::emit(snapshot);
    };

    CHECK(run_graph(emit_entries, {{{1, 10}}, {{1, 11}}, {{1, literal::REMOVE}}, {{1, 20}}})
              .equals({{{"key", 1}, {"value", 10}},
                       {{"key", 1}, {"value", 11}},
                       literal::SKIP,
                       {{"key", 1}, {"value", 20}}}));
}

TEST_CASE("collect emitted mapped dictionary values into a set and sum it",
          "[feature:operator][feature:map][feature:timed-scheduling][feature:collection-change]"
          "[series:tsd][scalar:set][transition:update][transition:removal]") {
    const auto increment = node([](i64 value) { return value + 1; });

    const auto total = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto entries = operators::emit(map(increment, values));
        const auto emitted_values = operators::getattr(entries, util::symbol{"value"});
        const auto collected = operators::collect[type<TS<Set<i64>>>](emitted_values);
        return operators::sum_(collected);
    };

    CHECK(run_graph(total, {{{1, 1}}, {{1, 2}}, {{1, literal::REMOVE}}, {{1, 4}}})
              .equals<i64>({2, 5, std::nullopt, 10}));
}

TEST_CASE("a nested collector emits its reset list snapshots",
          "[feature:operator][feature:nested-graph][feature:timed-scheduling][series:value]"
          "[scalar:list][transition:update]") {
    const auto collect_values = graph(
        "name"_n = "collect_values",
        [](wp<TS<i64>> value, wp<TS<bool>> reset) {
            return operators::collect[type<TS<List<i64>>>](value, "reset"_n = reset);
        });

    const auto emit_values = [&](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::emit(nested(collect_values, operators::const_(true), value, reset));
    };

    CHECK(run_graph(emit_values, {1, 2, 3, 4}, {literal::SKIP, literal::SKIP, false, literal::SKIP})
              .equals<i64>({1, 1, 2, 3, 3, 4}));
}

TEST_CASE("a switch selects a dictionary before map transforms and sums its values",
          "[feature:operator][feature:switch][feature:map][series:tsd]"
          "[scalar:i64][transition:update]") {
    const auto double_value = node([](i64 value) { return value * 2; });

    const auto total = [&](wp<TS<bool>> select_first, wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        const auto selected = switch_(select_first,
                                      cases{case_(true, [](auto left, auto) { return left; }),
                                            case_(false, [](auto, auto right) { return right; })},
                                      first, second);
        return operators::sum_("values"_n = map(double_value, selected));
    };

    CHECK(run_graph(total, {true, false, false, true, false},
                    {{{1, 1}, {2, 2}}, literal::SKIP, literal::SKIP,
                     {{1, 3}, {2, literal::REMOVE}}, literal::SKIP},
                    {literal::SKIP, {{1, 10}, {2, 20}}, {{1, 11}}, literal::SKIP, {{2, 25}}})
              .equals<i64>({6, 60, 62, 6, 72}));
}
