#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/nested.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;

TEST_CASE("map converts a scalar Dict returned through a nested graph",
          "[feature:map][feature:nested-graph][series:value][scalar:dict][transition:update]") {
    const auto identity =
        graph("name"_n = "identity_scalar_dictionary", [](wp<TS<Dict<i64, i64>>> values) { return values; });

    const auto increment = node([](i64 value) { return value + 1; });

    const auto materialize =
        node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values, wp<TS<bool>> run) {
        return materialize(map(increment, nested(identity, run, values)));
    };

    const auto actual = run_graph(tested, {{{1, 10}, {2, 20}}, {{1, 11}}}, {true, true});
    CHECK(actual.equals({{{1, 11}, {2, 21}}, {{1, 12}, {2, literal::REMOVE}}}));
}

TEST_CASE("map keeps per-key state across equal scalar Dict snapshots",
          "[feature:map][feature:nested-graph][feature:state][series:value][scalar:dict]"
          "[transition:update][transition:removal][transition:addition]") {
    const auto materialize =
        node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values) { return materialize(map(operators::count, values)); };

    // clang-format off
    CHECK(run_graph(tested, {
        {{1, 10}, {2, 20}},
        {{1, 10}, {2, 20}},
        {{1, 11}, {2, 20}},
        {{1, 11}},
        {{1, 11}, {2, 20}},
    }).equals({
        {{1, 1}, {2, 1}},
        literal::SKIP,
        {{1, 2}},
        {{2, literal::REMOVE}},
        {{2, 1}},
    }));
    // clang-format on
}

TEST_CASE("map follows scalar Dict reference changes through a nested switch",
          "[feature:map][feature:nested-graph][feature:switch][feature:reference]"
          "[series:value][scalar:dict][transition:update]") {
    const auto select_dictionary =
        graph("name"_n = "select_scalar_dictionary",
              [](wp<TS<bool>> use_first, wp<TS<Dict<i64, i64>>> first, wp<TS<Dict<i64, i64>>> second) {
                  return switch_(use_first,
                                 cases{case_(true, [](auto left, auto) { return left; }),
                                       case_(false, [](auto, auto right) { return right; })},
                                 first, second);
              });

    const auto increment = node([](i64 value) { return value + 1; });

    const auto materialize =
        node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TS<bool>> use_first, wp<TS<Dict<i64, i64>>> first, wp<TS<Dict<i64, i64>>> second,
                            wp<TS<bool>> run) {
        return materialize(map(increment, nested(select_dictionary, run, use_first, first, second)));
    };

    // clang-format off
    const auto actual = run_graph(tested,
        {true, false, false, true, true},
        {
            {{1, 10}, {2, 20}},
            literal::SKIP,
            literal::SKIP,
            {{1, 11}},
            {{2, 21}},
        },
        {
            {{1, 100}, {3, 300}},
            {{1, 101}},
            {{3, 310}},
            literal::SKIP,
            literal::SKIP,
        },
        {true, true, true, true, true});
    CHECK(actual.equals({
        {{1, 11}, {2, 21}},
        {{1, 102}, {2, literal::REMOVE}},
        {{1, literal::REMOVE}, {3, 311}},
        {{1, 12}, {3, literal::REMOVE}},
        {{1, literal::REMOVE}, {2, 22}},
    }));
    // clang-format on
}

TEST_CASE("map keeps an equal key branch when a scalar Dict reference changes",
          "[feature:map][feature:switch][feature:reference][feature:state]"
          "[series:value][scalar:dict][transition:update]") {
    const auto select_dictionary =
        graph("name"_n = "select_equal_scalar_dictionary",
              [](wp<TS<bool>> use_first, wp<TS<Dict<i64, i64>>> first, wp<TS<Dict<i64, i64>>> second) {
                  return switch_(use_first,
                                 cases{case_(true, [](auto left, auto) { return left; }),
                                       case_(false, [](auto, auto right) { return right; })},
                                 first, second);
              });

    const auto materialize =
        node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TS<bool>> use_first, wp<TS<Dict<i64, i64>>> first, wp<TS<Dict<i64, i64>>> second) {
        return materialize(map(operators::count, select_dictionary(use_first, first, second)));
    };

    // clang-format off
    CHECK(run_graph(tested,
        {true, false, false},
        {{{1, 10}}, literal::SKIP, literal::SKIP},
        {{{1, 10}}, literal::SKIP, {{1, 11}}})
        .equals({{{1, 1}}, literal::SKIP, {{1, 2}}}));
    // clang-format on
}

TEST_CASE("map combines scalar Dict and TSD updates for the union of keys",
          "[feature:map][feature:nested-graph][series:value][series:tsd][scalar:dict]"
          "[transition:addition][transition:update][transition:removal]") {
    const auto combine = node([](In<TS<i64>, optional> dictionary_value, In<TS<i64>, optional> time_series_value) {
        return (dictionary_value.valid() ? *dictionary_value : 0) +
               (time_series_value.valid() ? *time_series_value : 0);
    });

    const auto materialize =
        node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> dictionary, wp<TSD<i64, TS<i64>>> values) {
        return materialize(map(combine, dictionary, values));
    };

    // clang-format off
    const auto actual = run_graph(tested,
        {
            {{1, 10}, {2, 20}},
            literal::SKIP,
            {{1, 11}, {3, 30}},
            literal::SKIP,
        },
        {
            {{2, 200}, {3, 300}},
            {{2, 201}},
            {{2, literal::REMOVE}},
            {{1, 100}},
        });
    CHECK(actual.equals({
        {{1, 10}, {2, 220}, {3, 300}},
        {{2, 221}},
        {{1, 11}, {2, literal::REMOVE}, {3, 330}},
        {{1, 111}},
    }));
    // clang-format on
}
