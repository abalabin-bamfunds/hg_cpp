#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace hg;
using namespace hg::literals;

template <template <typename, auto...> typename Field> struct CollectionProjectionRow {
    Field<TSD<i64, TS<i64>>> values;
};

TEST_CASE("map projects a TSD field from each bundle and preserves nested removals",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][series:tsb]"
          "[shape:nested][transition:update][transition:removal]") {
    const auto project_values = graph([](wp<TSB<CollectionProjectionRow>> row) { return row->values; });

    const auto flatten = node([](In<TSD<Tuple<string, i64>, TS<i64>>> input,
                                 Out<TSD<Tuple<string, i64>, TS<i64>>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TSD<string, TSB<CollectionProjectionRow>>> rows) {
        return flatten(operators::collapse_keys(map(project_values, rows)));
    };

    const auto actual = run_graph(tested, {{{"A", {{"values", {{1, 10}, {2, 20}}}}}},
                                           {{"A", {{"values", {{1, 11}}}}}},
                                           {{"A", {{"values", {{2, literal::REMOVE}}}}}}});

    CHECK(actual.equals({{{{"A", 1}, 10}, {{"A", 2}, 20}}, {{{"A", 1}, 11}}, {{{"A", 2}, literal::REMOVE}}}));
}

template <template <typename, auto...> typename Field> struct MappedSetOutput {
    Field<TSS<i64>> values;
};

template <template <typename, auto...> typename Field> struct MappedSetRow {
    Field<TSS<i64>> selected;
};

TEST_CASE("map samples a held bundle TSS field when its explicit key arrives",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][series:tss][series:tsb]"
          "[shape:nested][transition:addition]") {
    const auto make_output =
        graph([](wp<TSB<MappedSetRow>> row) { return bundle<MappedSetOutput>("values"_n = row->selected); });

    const auto capture = node([](In<TSS<i64>> input, Out<TSS<i64>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TSD<string, TSB<MappedSetRow>>> rows, wp<TSS<string>> keys) {
        const auto mapped = map(make_output, rows, keys, "__key_set_arg__"_n = 1);
        return capture(operators::getattr(mapped[std::string{"A"}], util::symbol{"values"}));
    };

    CHECK(run_graph(tested, {{{"A", {{"selected", {1, 2}}}}}, literal::SKIP}, {literal::SKIP, {"A"}})
              .equals({literal::SKIP, {1, 2}}));
}

TEST_CASE("a projected TSS field waits for its explicitly selected mapped source",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][series:tss][series:tsb]"
          "[shape:nested][transition:addition]") {
    const auto make_output =
        graph([](wp<TSB<MappedSetRow>> row) { return bundle<MappedSetOutput>("values"_n = row->selected); });

    const auto capture = node([](In<TSS<i64>> input, Out<TSS<i64>> output) { output.copy_tick(input); });

    const auto tested = [&](wp<TSD<string, TSB<MappedSetRow>>> rows, wp<TSS<string>> keys) {
        const auto mapped = map(make_output, rows, keys, "__key_set_arg__"_n = 1);
        return capture(operators::getattr(mapped[std::string{"A"}], util::symbol{"values"}));
    };

    CHECK(run_graph(tested, {literal::SKIP, {{"A", {{"selected", {1, 2}}}}}}, {{"A"}, literal::SKIP})
              .equals({literal::SKIP, {1, 2}}));
}

template <template <typename, auto...> typename Field> struct AliasedCollections {
    Field<TSD<i64, TS<i64>>> first;
    Field<TSD<i64, TS<i64>>> second;
};

template <template <typename, auto...> typename Field> struct AliasMembership {
    Field<TS<bool>> first;
    Field<TS<bool>> second;
};

TEST_CASE("mapped bundle fields retain updates from their shared collection target",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][series:tsb]"
          "[shape:nested][transition:addition][transition:removal]") {
    const auto share_values = graph([](wp<TS<i64>>, wp<TSD<i64, TS<i64>>> values) {
        return bundle<AliasedCollections>("first"_n = values, "second"_n = values);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> branches, wp<TSD<i64, TS<i64>>> values) {
        const auto mapped = map(share_values, branches, pass_through(values));
        const auto row = mapped[i64{1}];
        return bundle<AliasMembership>(
            "first"_n =
                operators::contains_("collection"_n = operators::getattr(row, util::symbol{"first"}), "key"_n = i64{2}),
            "second"_n = operators::contains_("collection"_n = operators::getattr(row, util::symbol{"second"}),
                                              "key"_n = i64{2}));
    };

    CHECK(run_graph(tested, {{{1, 10}}, literal::SKIP, literal::SKIP}, {{{1, 100}}, {{2, 200}}, {{2, literal::REMOVE}}})
              .equals({{{"first", false}, {"second", false}},
                       {{"first", true}, {"second", true}},
                       {{"first", false}, {"second", false}}}));
}
