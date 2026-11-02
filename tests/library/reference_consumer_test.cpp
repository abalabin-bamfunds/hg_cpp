#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/mesh.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;

TEST_CASE("a selected dictionary item feeds its target value",
          "[feature:reference][series:tsd][series:value][binding:reference-to-value]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) { return values[i64{1}]; };

    CHECK(hg::run_graph(tested, {{{1, 10}}, {{1, 20}}}).equals<i64>({10, 20}));
}

template <template <typename, auto...> typename Field> struct reference_bundle {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

TEST_CASE("valid follows a selected dictionary item through invalidation and revalidation",
          "[feature:reference][feature:validity][series:tsd][transition:invalidation][transition:revalidation]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) { return operators::valid(values[i64{1}]); };

    CHECK(hg::run_graph(tested, {{{1, 10}}, {{1, literal::INVALID}}, {{1, 20}}})
              .equals<bool>({true, false, true}));
}

TEST_CASE("modified observes only selected dictionary item updates",
          "[feature:reference][series:tsd][series:value][activity:passive][binding:reference-to-value]"
          "[transition:update]") {
    const auto observe = node([](i64, In<TS<i64>, passive, optional> value) {
        return value.modified();
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<i64>> trigger) {
        return observe(trigger, values[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 20}}, {{2, 21}}, {{1, 11}}, {{1, 12}}},
                        {0, 1, 2, 3})
              .equals<bool>({true, false, true, true}));
}

TEST_CASE("lag follows a selected dictionary item and ignores sibling updates",
          "[feature:reference][feature:state][series:tsd][series:value][binding:reference-to-value]"
          "[transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) { return operators::lag(values[i64{1}]); };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 100}}, {{2, 101}}, {{1, 11}}, {{1, 12}}})
              .equals<i64>({std::nullopt, std::nullopt, 10, 11}));
}

TEST_CASE("take counts selected dictionary item ticks",
          "[feature:reference][feature:state][series:tsd][series:value][binding:reference-to-value]"
          "[transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::take(values[i64{1}], 2);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 100}}, {{2, 101}}, {{1, 11}}, {{1, 12}}})
              .equals<i64>({10, std::nullopt, 11, std::nullopt}));
}

TEST_CASE("drop skips the first selected dictionary item tick",
          "[feature:reference][feature:state][series:tsd][series:value][binding:reference-to-value]"
          "[transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::drop(values[i64{1}], 1);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}}, {{1, 11}}, {{2, 100}}, {{1, 12}}})
              .equals<i64>({std::nullopt, 11, std::nullopt, 12}));
}

TEST_CASE("dedup sees selected dictionary item values",
          "[feature:reference][feature:state][series:tsd][series:value][guarantee:deduplicated]"
          "[binding:reference-to-value][transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) { return operators::dedup(values[i64{1}]); };

    CHECK(hg::run_graph(tested, {{{1, 1}}, {{1, 1}}, {{1, 2}}, {{1, 2}}})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("an identity map element reference feeds lag",
          "[feature:reference][feature:map][feature:state][series:tsd][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return operators::lag(map([](wp<TS<i64>> value) { return value; }, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested, {{{1, 10}, {2, 100}}, {{2, 101}}, {{1, 11}}, {{1, 12}}})
              .equals<i64>({std::nullopt, std::nullopt, 10, 11}));
}

TEST_CASE("take counts identity map element reference ticks",
          "[feature:reference][feature:map][feature:state][series:tsd][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return operators::take(map([](wp<TS<i64>> value) { return value; }, values)[i64{1}], 2);
    };

    CHECK(hg::run_graph(tested, {{{1, 10}}, {{2, 100}}, {{1, 11}}, {{1, 12}}})
              .equals<i64>({10, std::nullopt, 11, std::nullopt}));
}

TEST_CASE("drop skips the first identity map element reference tick",
          "[feature:reference][feature:map][feature:state][series:tsd][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return operators::drop(map([](wp<TS<i64>> value) { return value; }, values)[i64{1}], 1);
    };

    CHECK(hg::run_graph(tested, {{{1, 10}}, {{1, 11}}, {{2, 100}}, {{1, 12}}})
              .equals<i64>({std::nullopt, 11, std::nullopt, 12}));
}

TEST_CASE("dedup sees identity map element reference values",
          "[feature:reference][feature:map][feature:state][series:tsd][series:value]"
          "[guarantee:deduplicated][binding:reference-to-value][transition:update]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return operators::dedup(map([](wp<TS<i64>> value) { return value; }, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested, {{{1, 1}}, {{1, 1}}, {{1, 2}}, {{1, 2}}})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("valid follows an identity map element reference",
          "[feature:reference][feature:map][feature:validity][series:tsd]"
          "[transition:invalidation][transition:revalidation]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return operators::valid(map([](wp<TS<i64>> value) { return value; }, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested, {{{1, 10}}, {{1, literal::INVALID}}, {{1, 20}}})
              .equals<bool>({true, false, true}));
}

TEST_CASE("modified observes identity map element updates without sibling changes",
          "[feature:reference][feature:map][series:tsd][series:value][activity:passive]"
          "[binding:reference-to-value][transition:update]") {
    const auto observe = node([](i64, In<TS<i64>, passive, optional> value) {
        return value.modified();
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<i64>> trigger) {
        return observe(trigger, map([](wp<TS<i64>> value) { return value; }, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested, {{{1, 10}, {2, 20}}, {{2, 21}}, {{1, 11}}}, {0, 1, 2})
              .equals<bool>({true, false, true}));
}

TEST_CASE("a conditional reference switch samples a distinct equal-valued target",
          "[feature:reference][feature:operator][feature:state][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto tested = [](wp<TS<bool>> condition, wp<TS<i64>> left, wp<TS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::count(selected);
    };

    CHECK(hg::run_graph(tested, {true, false, true}, {1, literal::SKIP, literal::SKIP},
                        {1, literal::SKIP, literal::SKIP})
              .equals<i64>({1, 2, 3}));
}

TEST_CASE("count ignores same-target switch turnover and resumes on target updates",
          "[feature:reference][feature:switch][feature:state][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto forward = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto tested = [&](wp<TS<i64>> branch, wp<TS<i64>> value) {
        const auto selected = switch_(branch,
                                      cases{case_(i64{0}, [&](auto input) { return forward(input); }),
                                            case_(i64{1}, [&](auto input) { return forward(input); })},
                                      value);
        return operators::count(selected);
    };

    CHECK(hg::run_graph(tested, {0, 1, 0, 1}, {10, literal::SKIP, literal::SKIP, 11})
              .equals<i64>({1, std::nullopt, std::nullopt, 2}));
}

TEST_CASE("lag ignores same-target switch turnover and keeps its prior event",
          "[feature:reference][feature:switch][feature:state][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto forward = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto tested = [&](wp<TS<i64>> branch, wp<TS<i64>> value) {
        const auto selected = switch_(branch,
                                      cases{case_(i64{0}, [&](auto input) { return forward(input); }),
                                            case_(i64{1}, [&](auto input) { return forward(input); })},
                                      value);
        return operators::lag(selected);
    };

    CHECK(hg::run_graph(tested, {0, 1, 0, 1}, {10, literal::SKIP, literal::SKIP, 11})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 10}));
}

TEST_CASE("keys_ follows a nested dictionary selected from a dictionary reference",
          "[feature:reference][feature:operator][series:tsd][series:tss][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return operators::keys_(values[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {}}},
                         {{1, {{10, 1}}}},
                         {{2, {{20, 2}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{20, 3}}}}})
              .equals({{}, {10}, literal::SKIP, {literal::REMOVED(10)}, literal::INVALID, {20}}));
}

TEST_CASE("len_ follows a nested dictionary selected from a dictionary reference",
          "[feature:reference][feature:operator][series:tsd][scalar:i64][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return operators::len_(values[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {}}},
                         {{1, {{10, 1}}}},
                         {{2, {{20, 2}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{20, 3}}}}})
              .equals<i64>({0, 1, std::nullopt, 0, std::nullopt, 1}));
}

TEST_CASE("is_empty follows a nested dictionary selected from a dictionary reference",
          "[feature:reference][feature:operator][series:tsd][series:value][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return operators::is_empty(values[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {}}},
                         {{1, {{10, 1}}}},
                         {{2, {{20, 2}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{20, 3}}}}})
              .equals<bool>({true, false, std::nullopt, true, std::nullopt, false}));
}

TEST_CASE("an identity map element reference feeds nested dictionary queries",
          "[feature:reference][feature:map][feature:operator][series:tsd][series:tss][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return operators::keys_(map([](wp<TSD<i64, TS<i64>>> value) { return value; }, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {}}},
                         {{1, {{10, 1}}}},
                         {{2, {{20, 2}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{20, 3}}}}})
              .equals({{}, {10}, literal::SKIP, {literal::REMOVED(10)}, literal::INVALID, {20}}));
}

TEST_CASE("len_ follows a nested dictionary selected from a map element reference",
          "[feature:reference][feature:map][feature:operator][series:tsd][scalar:i64][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto nested = map([](wp<TSD<i64, TS<i64>>> value) { return value; }, values)[i64{1}];
        return operators::len_(nested);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {}}},
                         {{1, {{10, 1}}}},
                         {{2, {{20, 2}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{20, 3}}}}})
              .equals<i64>({0, 1, std::nullopt, 0, std::nullopt, 1}));
}

TEST_CASE("is_empty follows a nested dictionary selected from a map element reference",
          "[feature:reference][feature:map][feature:operator][series:tsd][series:value][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto nested = map([](wp<TSD<i64, TS<i64>>> value) { return value; }, values)[i64{1}];
        return operators::is_empty(nested);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {}}},
                         {{1, {{10, 1}}}},
                         {{2, {{20, 2}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{20, 3}}}}})
              .equals<bool>({true, false, std::nullopt, true, std::nullopt, false}));
}

TEST_CASE("an inner value reference follows updates through a map of dictionaries",
          "[feature:reference][feature:map][series:tsd][series:value][shape:nested]"
          "[binding:reference-to-value][transition:update][transition:removal][transition:addition]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto nested = map([](wp<TSD<i64, TS<i64>>> value) { return value; }, values)[i64{1}];
        return nested[i64{10}];
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {{10, 10}}}},
                         {{1, {{10, 11}}}},
                         {{2, {{20, 20}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{10, 30}}}}})
              .equals<i64>({10, 11, std::nullopt, std::nullopt, std::nullopt, 30}));
}

TEST_CASE("a map can return a free list of its input items",
          "[feature:reference][feature:map][series:tsd][series:fixed-tsl][shape:nested]"
          "[representation:free][binding:reference-to-value][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> values) { return *values[0] + *values[1]; });

    const auto tested = [&](wp<TSD<i64, TSL<TS<i64>, 2>>> values) {
        const auto lists = map([](wp<TSL<TS<i64>, 2>> value) { return tsl(value[0], value[1]); }, values);
        return sum(lists[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {{0, 1}, {1, 10}}}, {2, {{0, 2}, {1, 20}}}},
                         {{1, {{0, 2}}}},
                         {{2, {{0, 3}}}},
                         {{1, {{1, 20}}}}})
              .equals<i64>({11, 12, std::nullopt, 22}));
}

TEST_CASE("count keeps a selected map reference across key removal and recreation",
          "[feature:reference][feature:map][feature:state][series:tsd][series:value]"
          "[binding:reference-to-value][transition:removal][transition:addition][transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::count(map([](wp<TS<i64>> value) { return value; }, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 20}},
                         {{2, 21}},
                         {{1, literal::REMOVE}, {2, 22}},
                         {{1, 11}},
                         {{2, 23}},
                         {{1, 12}}})
              .equals<i64>({1, std::nullopt, std::nullopt, 1, std::nullopt, 2}));
}

TEST_CASE("count resets after selected mesh key removal and restarts on recreation",
          "[feature:reference][feature:mesh][feature:state][series:tsd][series:value]"
          "[binding:reference-to-value][transition:removal][transition:addition][transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        const auto mapped = mesh([](wp<TS<i64>> value) { return value; }, values);
        return operators::count(mapped[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 20}},
                         {{2, 21}},
                         {{1, literal::REMOVE}, {2, 22}},
                         {{1, 11}},
                         {{2, 23}},
                         {{1, 12}}})
              .equals<i64>({1, std::nullopt, std::nullopt, 1, std::nullopt, 2}));
}

TEST_CASE("a free bundle reference keeps its selected field across switch reselection",
          "[feature:reference][feature:switch][feature:state][series:tsb][series:value]"
          "[representation:free][binding:reference-to-value][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_reference_bundle",
                                    [](wp<TSB<reference_bundle>> input) { return input; });

    const auto tested = [&](wp<TS<bool>> branch, wp<TS<i64>> left, wp<TS<i64>> right) {
        const auto fields = bundle<reference_bundle>(left, right);
        const auto selected = switch_(branch,
                                      cases{case_(true, pass_through), case_(false, pass_through)},
                                      fields);
        return operators::count(operators::getattr(selected, util::symbol{"left"}));
    };

    CHECK(hg::run_graph(tested,
                        {true, false, true, false},
                        {5, literal::SKIP, literal::SKIP, 6},
                        {10, literal::SKIP, literal::SKIP, literal::SKIP})
              .equals<i64>({1, std::nullopt, std::nullopt, 2}));
}

TEST_CASE("all_valid follows partial item validity through a switched free list",
          "[feature:reference][feature:switch][feature:validity][series:fixed-tsl]"
          "[representation:free][transition:invalidation][transition:revalidation]") {
    const auto pass_through = graph("name"_n = "pass_reference_list",
                                    [](wp<TSL<TS<i64>, 2>> input) { return input; });

    const auto tested = [&](wp<TS<bool>> branch, wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = tsl(first, second);
        return operators::all_valid(
            switch_(branch, cases{case_(true, pass_through), case_(false, pass_through)}, values));
    };

    CHECK(hg::run_graph(tested,
                        {true, false, true, false, true},
                        {1, literal::SKIP, literal::SKIP, 2, literal::SKIP},
                        {literal::INVALID, literal::SKIP, 3, literal::SKIP, literal::INVALID})
              .equals<bool>({false, std::nullopt, true, std::nullopt, false}));
}

TEST_CASE("switch branches build free lists from their current reference targets",
          "[feature:reference][feature:switch][series:fixed-tsl][series:value]"
          "[representation:free][transition:invalidation][transition:revalidation]") {
    const auto forward = graph("name"_n = "make_forward_reference_list",
                               [](wp<TS<i64>> first, wp<TS<i64>> second) { return tsl(first, second); });

    const auto reverse = graph("name"_n = "make_reverse_reference_list",
                               [](wp<TS<i64>> first, wp<TS<i64>> second) { return tsl(second, first); });

    const auto copy = node([](In<TSL<TS<i64>, 2>> values, Out<TSL<TS<i64>, 2>> output) {
        output.copy_tick(values);
    });

    const auto tested = [&](wp<TS<bool>> branch, wp<TS<i64>> first, wp<TS<i64>> second) {
        return copy(switch_(branch, cases{case_(true, forward), case_(false, reverse)}, first, second));
    };

    CHECK(hg::run_graph(tested,
                        {true, false, false, true, false},
                        {1, 2, literal::INVALID, 3, 4},
                        {10, 20, 21, 22, 23})
              .equals({{{0, 1}, {1, 10}},
                       {{0, 20}, {1, 2}},
                       {{0, 21}, {1, literal::INVALID}},
                       {{0, 3}, {1, 22}},
                       {{0, 23}, {1, 4}}}));
}

TEST_CASE("switch branches build free bundles from their current reference targets",
          "[feature:reference][feature:switch][series:tsb][series:value]"
          "[representation:free][schema:named][transition:invalidation][transition:revalidation]") {
    const auto forward = graph("name"_n = "make_forward_reference_bundle",
                               [](wp<TS<i64>> left, wp<TS<i64>> right) {
                                   return bundle<reference_bundle>(left, right);
                               });

    const auto reverse = graph("name"_n = "make_reverse_reference_bundle",
                               [](wp<TS<i64>> left, wp<TS<i64>> right) {
                                   return bundle<reference_bundle>(right, left);
                               });

    const auto copy = node([](In<TSB<reference_bundle>> fields, Out<TSB<reference_bundle>> output) {
        output.copy_tick(fields);
    });

    const auto tested = [&](wp<TS<bool>> branch, wp<TS<i64>> left, wp<TS<i64>> right) {
        return copy(switch_(branch, cases{case_(true, forward), case_(false, reverse)}, left, right));
    };

    CHECK(hg::run_graph(tested,
                        {true, false, false, true, false},
                        {1, 2, literal::INVALID, 3, 4},
                        {10, 20, 21, 22, 23})
              .equals({{{"left", 1}, {"right", 10}},
                       {{"left", 20}, {"right", 2}},
                       {{"left", 21}, {"right", literal::INVALID}},
                       {{"left", 3}, {"right", 22}},
                       {{"left", 23}, {"right", 4}}}));
}

TEST_CASE("a map can return a free bundle of its input fields",
          "[feature:reference][feature:map][series:tsd][series:tsb][shape:nested]"
          "[representation:free][schema:named][transition:update][transition:invalidation][transition:revalidation]") {
    const auto make_free_bundle = graph(
        "name"_n = "make_mapped_free_bundle",
        [](wp<TSB<reference_bundle>> input) { return bundle<reference_bundle>(input->left, input->right); });

    const auto copy = node([](In<TSB<reference_bundle>> input, Out<TSB<reference_bundle>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TSB<reference_bundle>>> values) {
        return copy(map(make_free_bundle, values)[i64{1}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {{"left", 1}, {"right", 10}}}, {2, {{"left", 2}, {"right", 20}}}},
                         {{1, {{"left", 2}}}},
                         {{2, {{"left", 3}, {"right", 30}}}},
                         {{1, {{"left", literal::INVALID}, {"right", 11}}}},
                         {{1, {{"left", 4}}}}})
              .equals({{{"left", 1}, {"right", 10}},
                       {{"left", 2}},
                       literal::SKIP,
                       {{"left", literal::INVALID}, {"right", 11}},
                       {{"left", 4}}}));
}

TEST_CASE("map switch resets one key count on removal and preserves the other",
          "[feature:reference][feature:map][feature:switch][feature:state][series:tsd][series:tsb]"
          "[binding:reference-to-value][transition:update]") {
    const auto identity = graph("name"_n = "reference_count_map_identity",
                                [](wp<TS<i64>> value) { return value; });

    const auto tested = [&](wp<TS<bool>> branch, wp<TSD<i64, TS<i64>>> first,
                            wp<TSD<i64, TS<i64>>> second) {
        const auto first_map = map(identity, first);
        const auto second_map = map(identity, second);
        const auto selected = switch_(branch,
                                     cases{case_(true, [](auto first_map, auto) { return first_map; }),
                                           case_(false, [](auto, auto second_map) { return second_map; })},
                                     first_map, second_map);
        return bundle<reference_bundle>(operators::count(selected[i64{1}]),
                                       operators::count(selected[i64{2}]));
    };

    CHECK(hg::run_graph(tested,
                        {true, true, true, true, false, true},
                        {{{1, 10}, {2, 100}}, {{1, 11}}, {{2, literal::REMOVE}}, {{2, 101}}, literal::SKIP,
                         literal::SKIP},
                        {{{1, 20}, {2, 200}}, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP,
                         literal::SKIP})
              .equals({{{"left", 1}, {"right", 1}},
                       {{"left", 2}},
                       {{"right", literal::INVALID}},
                       {{"right", 1}},
                       {{"left", 3}, {"right", 2}},
                       {{"left", 4}, {"right", 3}}}));
}

TEST_CASE("map switch keeps per-key lag state independent through removal and recreation",
          "[feature:reference][feature:map][feature:switch][feature:state][series:tsd][series:tsb]"
          "[binding:reference-to-value][transition:update]") {
    const auto identity = graph("name"_n = "reference_lag_map_identity",
                                [](wp<TS<i64>> value) { return value; });

    const auto tested = [&](wp<TS<bool>> branch, wp<TSD<i64, TS<i64>>> first,
                            wp<TSD<i64, TS<i64>>> second) {
        const auto first_map = map(identity, first);
        const auto second_map = map(identity, second);
        const auto selected = switch_(branch,
                                     cases{case_(true, [](auto first_map, auto) { return first_map; }),
                                           case_(false, [](auto, auto second_map) { return second_map; })},
                                     first_map, second_map);
        return bundle<reference_bundle>(operators::lag(selected[i64{1}]),
                                       operators::lag(selected[i64{2}]));
    };

    const auto actual = hg::run_graph(
        tested,
        {true, true, true, true, false, true},
        {{{1, 10}, {2, 100}}, {{1, 11}}, {{2, literal::REMOVE}}, {{2, 101}}, literal::SKIP, literal::SKIP},
        {{{1, 20}, {2, 200}}, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP});

    CHECK(actual.equals({literal::SKIP,
                         {{"left", 10}},
                         {{"right", 100}},
                         {{"right", literal::INVALID}},
                         {{"left", 11}, {"right", 101}},
                         {{"left", 20}, {"right", 200}}}));
}

TEST_CASE("valid follows an inner item reference through a map lifecycle",
          "[feature:reference][feature:map][feature:validity][series:tsd][series:value][shape:nested]"
          "[transition:invalidation][transition:revalidation][transition:removal][transition:addition]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto nested = map([](wp<TSD<i64, TS<i64>>> value) { return value; }, values)[i64{1}];
        return operators::valid(nested[i64{10}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {{10, 10}}}},
                         {{2, {{20, 20}}}},
                         {{1, {{10, literal::REMOVE}}}},
                         {{1, {{10, 11}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{10, 12}}}}})
              .equals<bool>({true, std::nullopt, false, true, false, true}));
}

TEST_CASE("modified observes nested map item updates and ignores sibling updates",
          "[feature:reference][feature:map][series:tsd][series:value][activity:passive][shape:nested]"
          "[binding:reference-to-value][transition:update]") {
    const auto observe = node([](i64, In<TS<i64>, passive, optional> value) { return value.modified(); });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values, wp<TS<i64>> trigger) {
        const auto nested = map([](wp<TSD<i64, TS<i64>>> value) { return value; }, values)[i64{1}];
        return observe(trigger, nested[i64{10}]);
    };

    CHECK(hg::run_graph(tested,
                        {{{1, {{10, 10}}}},
                         {{2, {{20, 20}}}},
                         {{1, {{10, 11}}}},
                         {{2, {{20, 21}}}}},
                        {0, 1, 2, 3})
              .equals<bool>({true, false, true, false}));
}
