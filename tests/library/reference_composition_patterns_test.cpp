#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/mesh.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/scalar.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;

TEST_CASE("a map-selected dictionary item feeds its target value",
          "[feature:reference][feature:map][series:tsd][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto inner_dictionary = graph([](wp<TSD<i64, TS<i64>>> values) { return values; });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return map(inner_dictionary, values)[i64{1}][i64{10}];
    };

    CHECK(run_graph(tested, {{{1, {{10, 1}}}}, {{1, {{10, 2}}}}}).equals<i64>({1, 2}));
}

TEST_CASE("collect appends events from a nested map-selected reference",
          "[feature:reference][feature:map][feature:operator][series:tsd][series:value]"
          "[scalar:list][transition:update]") {
    const auto inner_dictionary = graph([](wp<TSD<i64, TS<i64>>> values) { return values; });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto selected = map(inner_dictionary, values)[i64{1}][i64{10}];
        return operators::collect[type<TS<List<i64>>>](selected);
    };

    CHECK(run_graph(tested,
                    {{{1, {{10, 10}}}, {2, {{20, 20}}}},
                     {{2, {{20, 21}}}},
                     {{1, {{10, 11}}}},
                     {{1, {{10, literal::INVALID}}}},
                     {{1, {{10, 12}}}}})
              .equals({{10}, literal::SKIP, {10, 11}, literal::SKIP, {10, 11, 12}}));
}

TEST_CASE("merge forwards updates from two items selected through a nested map",
          "[feature:reference][feature:map][feature:merge][series:tsd][series:value]"
          "[binding:reference-to-value][transition:update]") {
    const auto inner_dictionary = graph([](wp<TSD<i64, TS<i64>>> values) { return values; });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto selected = map(inner_dictionary, values)[i64{1}];
        return operators::merge(selected[i64{10}], selected[i64{20}]);
    };

    CHECK(run_graph(tested,
                    {{{1, {{10, 1}}}},
                     {{1, {{20, 2}}}},
                     {{1, {{10, 3}}}},
                     {{1, {{20, 4}}}}})
              .equals<i64>({1, 2, 3, 4}));
}

TEST_CASE("convert a dictionary selected through a nested map to a scalar snapshot",
          "[feature:reference][feature:map][feature:operator][series:tsd][scalar:dict]"
          "[transition:update][transition:removal]") {
    const auto inner_dictionary = graph([](wp<TSD<i64, TS<i64>>> values) { return values; });

    const auto tested = graph([&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        const auto selected = map(inner_dictionary, values)[i64{1}];
        return operators::convert[type<TS<Dict<i64, i64>>>](selected);
    });

    CHECK(run_graph(tested,
                    {{{1, {{10, 1}, {20, 2}}}, {2, {{30, 3}}}},
                     {{1, {{10, 4}}}},
                     {{2, {{30, 5}}}},
                     {{1, {{20, literal::REMOVE}}}},
                     {{1, {{10, 6}}}}})
              .equals({{{10, 1}, {20, 2}},
                       {{10, 4}, {20, 2}},
                       literal::SKIP,
                       {{10, 4}},
                       {{10, 6}}}));
}

template <template <typename, auto...> typename Field> struct reference_point {
    Field<i64> count;
    Field<string> label;
};

TEST_CASE("a compound scalar field follows a mapped item reference",
          "[feature:reference][feature:map][feature:getattr][series:tsd][series:value]"
          "[scalar:hierarchical][schema:named][transition:update]") {
    const auto identity = graph([](wp<TS<Bundle<reference_point>>> value) { return value; });

    const auto tested = [&](wp<TSD<i64, TS<Bundle<reference_point>>>> values) {
        return operators::getattr(map(identity, values)[i64{1}], util::symbol{"count"});
    };

    CHECK(run_graph(tested,
                    {{{1, {{"count", 1}, {"label", "one"}}}},
                     {{1, {{"count", 2}, {"label", "two"}}}},
                     {{1, {{"count", 3}, {"label", "three"}}}}})
              .equals<i64>({1, 2, 3}));
}

TEST_CASE("collect scalar ticks routed through mesh item references",
          "[feature:reference][feature:mesh][feature:operator][feature:state]"
          "[series:tsd][series:value][scalar:list][transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        const auto items = mesh([](wp<TS<i64>> value) { return value; }, values);
        const auto merged = operators::merge(items[i64{1}], items[i64{2}]);
        return operators::collect[type<TS<List<i64>>>](merged);
    };

    CHECK(run_graph(tested, {{{1, 10}}, {{2, 20}}, {{1, 11}}})
              .equals({{10}, {10, 20}, {10, 20, 11}}));
}

TEST_CASE("convert a directly selected dictionary reference to a scalar snapshot",
          "[feature:reference][feature:operator][series:tsd][scalar:dict]") {
    const auto tested = graph([](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return operators::convert[type<TS<Dict<i64, i64>>>](values[i64{1}]);
    });

    CHECK(run_graph(tested, {{{1, {{10, 1}, {20, 2}}}}, {{1, {{10, 4}}}}})
              .equals({{{10, 1}, {20, 2}}, {{10, 4}, {20, 2}}}));
}

TEST_CASE("sum a free fixed list built from items selected through a map",
          "[feature:reference][feature:map][feature:operator][series:tsd][series:fixed-tsl]"
          "[representation:free][scalar:i64][transition:update]") {
    const auto make_list = graph([](wp<TSD<i64, TS<i64>>> values) {
        return tsl(values[i64{10}], values[i64{20}]);
    });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> values) {
        return operators::sum_(map(make_list, values)[i64{1}]);
    };

    CHECK(run_graph(tested,
                    {{{1, {{10, 1}, {20, 10}}}},
                     {{1, {{10, 2}}}},
                     {{1, {{20, 20}}}},
                     {{1, {{10, literal::INVALID}}}},
                     {{1, {{10, 3}}}}})
              .equals<i64>({11, 12, 22, std::nullopt, 23}));
}

template <template <typename, auto...> typename Field> struct nested_reference_bundle {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

TEST_CASE("recombine fields from a mapped bundle reference and observe each field",
          "[feature:reference][feature:map][series:tsd][series:tsb][series:value]"
          "[representation:free][schema:named][feature:state][transition:update]") {
    const auto swap_fields = graph("name"_n = "swap_mapped_reference_fields",
                                   [](wp<TSB<nested_reference_bundle>> values) {
                                       return bundle<nested_reference_bundle>(values->right, values->left);
                                   });

    const auto tested = [&](wp<TSD<i64, TSB<nested_reference_bundle>>> values) {
        const auto selected = map(swap_fields, values)[i64{1}];
        return bundle<nested_reference_bundle>(
            operators::count(operators::getattr(selected, util::symbol{"left"})),
            operators::count(operators::getattr(selected, util::symbol{"right"})));
    };

    CHECK(run_graph(tested,
                    {{{1, {{"left", 10}, {"right", 100}}},
                      {2, {{"left", 20}, {"right", 200}}}},
                     {{2, {{"left", 21}}}},
                     {{1, {{"left", 11}}}},
                     {{1, {{"right", 101}}}}})
              .equals({{{"left", 1}, {"right", 1}},
                       literal::SKIP,
                       {{"right", 2}},
                       {{"left", 2}}}));
}

TEST_CASE("a switch ignores inactive dictionary changes and samples a re-added nested item",
          "[feature:reference][feature:map][feature:switch][feature:state]"
          "[series:tsd][series:value][binding:reference-to-value][transition:update]"
          "[transition:removal][transition:addition]") {
    const auto inner_dictionary = graph([](wp<TSD<i64, TS<i64>>> values) { return values; });

    const auto tested = [&](wp<TS<bool>> choose_first, wp<TSD<i64, TSD<i64, TS<i64>>>> first,
                            wp<TSD<i64, TSD<i64, TS<i64>>>> second) {
        const auto selected_values = switch_(choose_first,
                                             cases{case_(true, [](auto left, auto) { return left; }),
                                                   case_(false, [](auto, auto right) { return right; })},
                                             first, second);
        const auto selected_item = map(inner_dictionary, selected_values)[i64{1}][i64{10}];
        return tsl(selected_item, operators::count(selected_item));
    };

    CHECK(run_graph(tested,
                    {true, false, false, false, true, true, false},
                    {{{1, {{10, 1}}}},
                     {{1, {{10, 2}}}},
                     {{1, {{10, 3}}}},
                     {{1, {{10, literal::REMOVE}}}},
                     {{1, {{10, 4}}}},
                     literal::SKIP,
                     {{1, {{10, 5}}}}},
                    {{{1, {{10, 10}}}},
                     {{1, {{10, 20}}}},
                     literal::SKIP,
                     {{1, {{10, 22}}}},
                     {{1, {{10, 23}}}},
                     {{1, {{10, 24}}}},
                     {{1, {{10, 25}}}}})
              .equals({{{0, 1}, {1, 1}},
                       {{0, 20}, {1, 2}},
                       literal::SKIP,
                       {{0, 22}, {1, 3}},
                       {{0, 4}, {1, 4}},
                       literal::SKIP,
                       {{0, 25}, {1, 5}}}));
}
