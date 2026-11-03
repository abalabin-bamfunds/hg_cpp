#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/reduce.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>

namespace {

using namespace hg;
using namespace hg::literals;

const auto add_values = node([](i64 left, i64 right) { return left + right; });

const auto add_graph = graph("name"_n = "add_reduce_composition_values",
                             [](wp<TS<i64>> left, wp<TS<i64>> right) {
                                 return add_values(left, right);
                             });

const auto add_or_zero = graph("name"_n = "add_optional_inner_values",
                               [](wp<TS<i64>> left, wp<TS<i64>> right) {
                                   return add_values(operators::default_(left, i64{0}),
                                                     operators::default_(right, i64{0}));
                               });

const auto merge_inner = graph("name"_n = "merge_inner_dictionaries",
                               [](wp<TSD<i64, TS<i64>>> left,
                                   wp<TSD<i64, TS<i64>>> right) {
                                   return map(add_or_zero, left, right);
                               });

TEST_CASE("reduce merges nested dictionaries by key before reducing their values",
          "[feature:reduce][feature:map][series:tsd][shape:nested]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto combined = reduce(merge_inner, values);
        return reduce(add_graph, combined, "default"_n = i64{0});
    };

    const auto actual = run_graph(
        tested, {{{1, {{1, 10}, {2, 20}}}, {2, {{2, 3}, {3, 4}}}}});

    CHECK(actual.equals<i64>({37}));
}

template <template <typename, auto...> typename Field> struct ReducedPair {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

TEST_CASE("reduce uses a bundle default only while its TSD has no valid items",
          "[feature:reduce][series:tsd][series:tsb][series:reference][shape:nested]"
          "[transition:addition][transition:removal][transition:update]") {
    const auto add_bundles = graph("name"_n = "add_reduce_composition_bundles",
                                   [=](wp<TSB<ReducedPair>> left, wp<TSB<ReducedPair>> right) {
                                       return bundle<TSB<ReducedPair>>(
                                           "left"_n = add_values(left->left, right->left),
                                           "right"_n = add_values(left->right, right->right));
                                   });

    const auto tested = [=](wp<TSD<i64, TSB<ReducedPair>>> values,
                            wp<TSB<ReducedPair>> fallback) {
        const auto reduced = reduce(add_bundles, values, "default"_n = fallback).as<TSB<ReducedPair>>();
        return add_values(reduced->left, reduced->right);
    };

    const auto actual = run_graph(
        tested,
        {{},
         {{1, {{"left", 4}, {"right", 5}}}},
         {{2, {{"left", 6}, {"right", 7}}}},
         {{3, {{"left", 8}, {"right", 9}}}},
         {{1, literal::REMOVE}},
         {{2, literal::REMOVE}, {3, literal::REMOVE}},
         {}},
        {{{"left", 2}, {"right", 3}},
         {{"left", 20}, {"right", 30}},
         {{"left", 200}, {"right", 300}},
         {{"left", 2'000}, {"right", 3'000}},
         {{"left", 20'000}, {"right", 30'000}},
         {{"left", 100}, {"right", 200}},
         {{"left", 10}, {"right", 20}}});

    CHECK(actual.equals<i64>({5, 9, 22, 39, 30, 300, 30}));
}

TEST_CASE("nested dictionary reduce retires mapped reducer branches as the tree shrinks",
          "[feature:reduce][feature:map][feature:getitem][feature:switch][series:tsd][series:reference]"
          "[shape:deeply-nested][transition:removal][transition:addition]") {
    const auto is_non_zero = node([](i64 value) { return value != 0; });

    const auto reduce_non_zero = graph("name"_n = "reduce_non_zero_inner_values",
                                       [=](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
                                           const auto combined = reduce(merge_inner, values);
                                           const auto non_zero = map(is_non_zero, combined);
                                           const auto selected = combined[operators::keys_where_true(non_zero)];
                                           return reduce(add_graph, selected, "default"_n = i64{0});
                                       });

    const auto tested = [=](wp<TSD<i64, TSD<i64, TS<i64>>>> values,
                            wp<TS<bool>> select_second) {
        return switch_(select_second,
                       cases{case_(true, reduce_non_zero), case_(false, reduce_non_zero)},
                       values);
    };

    const auto values = ticks(
        {{{1, {{1, 1}, {2, 2}}}, {2, {{1, 2}, {2, 3}}}, {3, {{1, 3}, {2, 4}}},
          {4, {{1, 4}, {2, 5}}}, {5, {{1, 5}, {2, 6}}}, {6, {{1, 6}, {2, 7}}},
          {7, {{1, 7}, {2, 8}}}, {8, {{1, 8}, {2, 9}}}, {9, {{1, 9}, {2, 10}}},
          {10, {{1, 10}, {2, 11}}}, {11, {{1, 11}, {2, 12}}}, {12, {{1, 12}, {2, 13}}},
          {13, {{1, 13}, {2, 14}}}, {14, {{1, 14}, {2, 15}}}, {15, {{1, 15}, {2, 16}}},
          {16, {{1, 16}, {2, 17}}}, {17, {{1, 17}, {2, 18}}}},
         {{1, literal::REMOVE}, {2, literal::REMOVE}, {3, literal::REMOVE},
          {4, literal::REMOVE}, {5, literal::REMOVE}, {6, literal::REMOVE},
          {7, literal::REMOVE}, {8, literal::REMOVE}, {9, literal::REMOVE},
          {10, literal::REMOVE}, {11, literal::REMOVE}},
         {{1, {{1, 100}}}},
         {{12, literal::REMOVE}, {13, literal::REMOVE}, {14, literal::REMOVE},
          {15, literal::REMOVE}, {16, literal::REMOVE}, {17, literal::REMOVE}},
         {{1, literal::REMOVE}},
         {{18, {{1, 10}, {2, 20}}}, {19, {{1, 1}}}},
         {{19, literal::REMOVE}},
         {{18, literal::REMOVE}}});
    const std::initializer_list<std::optional<i64>> expected{323, 180, 280, 100, 0, 31, 30, 0};
    const auto direct = [=](wp<TSD<i64, TSD<i64, TS<i64>>>> values) { return reduce_non_zero(values); };

    CHECK(run_graph(direct, values).equals<i64>(expected));
    CHECK(run_graph(tested, values, ticks({true, true, false, false, false, true, true, true}))
              .equals<i64>(expected));
}

} // namespace
