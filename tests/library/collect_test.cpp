#include <bg/library.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bg;
using namespace bg::literals;

TEST_CASE("collect appends each valid event and preserves history across invalidation",
          "[feature:operator][series:value][scalar:list][validity:optional]"
          "[transition:first-tick][transition:update][transition:invalidation][transition:revalidation]") {
    const auto collect = [](wp<TS<i64>> value) {
        return operators::collect[type<TS<List<i64>>>](value);
    };

    CHECK(bg::run_graph(collect, {1, 1, literal::INVALID, 2})
              .equals({{1}, {1, 1}, literal::SKIP, {1, 1, 2}}));
}

TEST_CASE("collect resets a scalar list while its source is invalid and appends simultaneous data",
          "[feature:operator][series:value][scalar:list][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto collect = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::collect[type<TS<List<i64>>>](value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(collect,
                              {1, literal::INVALID, literal::SKIP, 2},
                              {literal::SKIP, literal::SKIP, false, literal::SKIP})
              .equals({{1}, literal::SKIP, {}, {2}}));
    CHECK(bg::run_graph(collect, {1, 2}, {literal::SKIP, false}).equals({{1}, {2}}));
}

TEST_CASE("collect scalar sets publish equal ordinary ticks and deduplicate requested outputs",
          "[feature:operator][series:value][scalar:set][guarantee:ordinary][guarantee:deduplicated]"
          "[transition:first-tick][transition:update]") {
    const auto ordinary = [](wp<TS<i64>> value) {
        return operators::collect[type<TS<Set<i64>>>](value);
    };
    const auto deduplicated = [](wp<TS<i64>> value) {
        return operators::collect[type<TS<Set<i64>, TimeSeriesGuarantee::deduplicated>>](value);
    };

    CHECK(bg::run_graph(ordinary, {1, 1, 2}).equals({{1}, {1}, {1, 2}}));
    CHECK(bg::run_graph(deduplicated, {1, 1, 2}).equals({{1}, literal::SKIP, {1, 2}}));
}

TEST_CASE("collect into TSS uses membership deltas and reset clears before inserting",
          "[feature:operator][series:tss][scalar:integer][collection-change][validity:optional]"
          "[transition:first-tick][transition:update]") {
    const auto collect = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::collect[type<TSS<i64>>](value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(collect,
                              {1, 1, 2, literal::INVALID, literal::SKIP, 2},
                              {literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP,
                               false, literal::SKIP})
              .equals({{1}, literal::SKIP, {2}, literal::SKIP, {literal::REMOVED(1), literal::REMOVED(2)}, {2}}));
}

TEST_CASE("collect scalar dictionaries replace values only on value ticks",
          "[feature:operator][series:value][scalar:dict][validity:optional]"
          "[activity:passive][transition:first-tick][transition:update]") {
    const auto collect = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return operators::collect[type<TS<Dict<i64, i64>>>]("key"_n = key, "value"_n = value);
    };

    CHECK(bg::run_graph(collect, {1, 1, 2, literal::SKIP}, {10, 10, literal::SKIP, 20})
              .equals({{{1, 10}}, {{1, 10}}, literal::SKIP, {{1, 10}, {2, 20}}}));
}

TEST_CASE("collect scalar dictionaries ignore value ticks without a valid key and reset while invalid",
          "[feature:operator][series:value][scalar:dict][validity:optional]"
          "[transition:first-tick][transition:update][transition:invalidation]") {
    const auto collect = [](wp<TS<i64>> key, wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::collect[type<TS<Dict<i64, i64>>>]("key"_n = key, "value"_n = value,
                                                            "reset"_n = reset);
    };

    CHECK(bg::run_graph(collect,
                              {literal::INVALID, 2, 1, literal::SKIP},
                              {5, 6, 10, literal::INVALID},
                              {literal::SKIP, literal::SKIP, literal::SKIP, false})
              .equals({literal::SKIP, {{2, 6}}, {{1, 10}, {2, 6}}, {}}));
    CHECK(bg::run_graph(collect, {1, 2}, {10, 20}, {literal::SKIP, false})
              .equals({{{1, 10}}, {{2, 20}}}));
}

TEST_CASE("collect into TSD inserts and replaces values with normal dictionary deltas",
          "[feature:operator][series:tsd][scalar:integer][collection-change][validity:optional]"
          "[activity:passive][transition:first-tick][transition:update]") {
    const auto collect = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return operators::collect[type<TSD<i64, TS<i64>>>]("key"_n = key, "value"_n = value);
    };

    CHECK(bg::run_graph(collect, {1, 1, 1, 2}, {10, 11, literal::SKIP, 20})
              .equals({{{1, 10}}, {{1, 11}}, literal::SKIP, {{2, 20}}}));
}

TEST_CASE("collect into TSD resets while invalid and publishes simultaneous key and value",
          "[feature:operator][series:tsd][scalar:integer][collection-change][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto collect = [](wp<TS<i64>> key, wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::collect[type<TSD<i64, TS<i64>>>]("key"_n = key, "value"_n = value,
                                                            "reset"_n = reset);
    };

    CHECK(bg::run_graph(collect, {1, literal::SKIP, 2}, {10, literal::INVALID, 20},
                              {literal::SKIP, false, false})
              .equals({{{1, 10}}, {{1, literal::REMOVE}}, {{2, 20}}}));
}

TEST_CASE("collect reads changes through a selected TSD reference",
          "[feature:operator][feature:reference][series:value][scalar:list][binding:reference-to-value]"
          "[transition:first-tick][transition:update]") {
    const auto collect = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::collect[type<TS<List<i64>>>](operators::getitem(values, i64{1}));
    };

    CHECK(bg::run_graph(collect, bg::input<TSD<i64, TS<i64>>>({{{1, 10}}, {{1, 20}}}))
              .equals({{10}, {10, 20}}));
}

TEST_CASE("collect requires a compatible explicit accumulating output type",
          "[feature:operator][feature:wiring][series:value][scalar:list]") {
    const auto wrong_shape = [](wp<TS<i64>> value) {
        return operators::collect[type<TS<i64>>](value);
    };
    const auto final_output = [](wp<TS<i64>> value) {
        return operators::collect[type<TS<List<i64>, TimeSeriesGuarantee::final>>](value);
    };
    const auto final_item = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return operators::collect[type<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>>]("key"_n = key,
                                                                                       "value"_n = value);
    };
    const auto inferred_output = [](wp<TS<i64>> value) { return operators::collect(value); };

    CHECK_THROWS_AS(bg::run_graph(wrong_shape, {1}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(final_output, {1}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(final_item, {1}, {1}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(inferred_output, {1}), WiringError);
}
