#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace bg;
using namespace bg::literals;

namespace {

template <template <typename, auto...> typename Field> struct length_bundle {
    Field<TS<i64>> first;
    Field<TS<double>> second;
};

const auto final_keys = node([](Out<TSS<i64, TimeSeriesGuarantee::final>> output) { output.apply({1, 2}); });

} // namespace

TEST_CASE("len_ of a TSS follows key membership", "[feature:operator][series:tss][scalar:i64][shape:single-node]") {
    const auto graph = [](wp<TSS<i64>> value) { return operators::len_("value"_n = value); };

    CHECK(testing::test_graph(graph, {{1}, {2, literal::REMOVED(1)}}).equals<i64>({1, 1}));
}

TEST_CASE("len_ of a fixed TSL is a wiring-time constant",
          "[feature:operator][series:fixed-tsl][scalar:i64][shape:single-node][guarantee:const]") {
    const auto graph = [] {
        const auto values = std::array{operators::const_(i64{1}), operators::const_(i64{2}), operators::const_(i64{3})};
        return operators::len_("value"_n = tsl(values));
    };

    CHECK(testing::test_graph(graph).equals<i64>({3}));

    auto wiring = GraphWiring{};
    auto result = WiringPoint{};
    {
        auto context = WiringContext{wiring};
        result = graph();
    }
    result = wiring.complete_pending_wiring(result);
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 0);
}

TEST_CASE("len_ rejects a TSL with sparse item metadata",
          "[feature:operator][feature:wiring][series:fixed-tsl][scalar:i64][validity:required]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &sparse_type = time_series_list_type(TS<i64>::type(), std::dynamic_extent,
                                                        TimeSeriesGuarantee::ordinary);
        const auto sparse_input = wiring.borrowed_input(sparse_type);

        CHECK_THROWS_AS(operators::len_("value"_n = sparse_input), WiringError);
    });
}

TEST_CASE("len_ of a fixed TSB is a wiring-time constant",
          "[feature:operator][series:tsb][schema:named][scalar:i64][shape:single-node][guarantee:const]") {
    const auto graph = [] {
        const auto values = bundle<TSB<length_bundle>>(i64{1}, 2.0);
        return operators::len_("value"_n = values);
    };

    CHECK(testing::test_graph(graph).equals<i64>({2}));
}

TEST_CASE("len_ does not turn an invalid TSS into a valid empty set",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TSS<i64>> values) { return operators::len_("value"_n = values); };

    CHECK(testing::test_graph(graph, {literal::SKIP, {}}).equals<i64>({std::nullopt, 0}));
}

TEST_CASE("contains_ on a TSS queries the current key set",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(testing::test_graph(graph, {{1}, {2, literal::REMOVED(1)}, {1}}).equals<bool>({true, false, true}));
}

TEST_CASE("contains_ distinguishes an invalid TSS from a valid empty TSS",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(testing::test_graph(graph, {literal::SKIP, {}}).equals<bool>({std::nullopt, false}));
}

TEST_CASE("contains_ on a TSS accepts a time-series key",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary]") {
    const auto graph = [](wp<TSS<i64>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    CHECK(testing::test_graph(graph, {{1}, {2}}, {1, 2}).equals<bool>({true, true}));
}

TEST_CASE("contains_ follows a time-series key through invalidation and revalidation",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSS<i64>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    CHECK(testing::test_graph(graph, {{1, 2}, {1, 2}, {1, 2}}, testing::input<TS<i64>>({1, literal::INVALID, 2}))
              .equals<bool>({true, std::nullopt, true}));
}

TEST_CASE("contains_ follows a selected TSS reference through retargeting and invalidation",
          "[feature:operator][feature:reference][series:tss][scalar:i64][binding:reference-to-value]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto query = [](wp<TS<bool>> condition, wp<TSS<i64>> left, wp<TSS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::contains_("collection"_n = selected, "key"_n = i64{1});
    };

    CHECK(testing::test_graph(query, testing::input<TS<bool>>({true, false, literal::INVALID, true}),
                              {{1}, {2}, {3}, {1}}, {{2}, {2}, {3}, {3}})
              .equals<bool>({true, false, std::nullopt, true}));

    const auto observe_validity = [](wp<TS<bool>> condition, wp<TSS<i64>> left, wp<TSS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::valid(operators::contains_("collection"_n = selected, "key"_n = i64{1}));
    };
    CHECK(testing::test_graph(observe_validity, testing::input<TS<bool>>({true, false, literal::INVALID, true}),
                              {{1}, {2}, {3}, {1}}, {{2}, {2}, {3}, {3}})
              .equals<bool>({true, std::nullopt, false, true}));
}

TEST_CASE("contains_ propagates a final TSS guarantee",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:final]") {
    const auto graph = [] {
        const auto result = operators::contains_("collection"_n = final_keys(), "key"_n = i64{1});
        CHECK(&result.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        return result;
    };

    CHECK(testing::test_graph(graph).equals<bool>({true}));
}

TEST_CASE("contains_ combines a final TSS with an ordinary time-series key",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary][transition:update]") {
    const auto graph = [](wp<TS<i64>> key) {
        const auto result = operators::contains_("collection"_n = final_keys(), "key"_n = key);
        CHECK(&result.type() == &TS<bool>::type());
        return result;
    };

    CHECK(testing::test_graph(graph, {1, 2, 3}).equals<bool>({true, true, false}));
}

TEST_CASE("len_ forwards equal sizes for ordinary inputs and suppresses them for deduplicated inputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][guarantee:deduplicated]"
          "[transition:update]") {
    const auto ordinary = [](wp<TSS<i64>> values) { return operators::len_("value"_n = values); };
    const auto deduplicated = [](wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> values) {
        return operators::len_("value"_n = values);
    };

    CHECK(testing::test_graph(ordinary,
                              {{1, 2}, {literal::REMOVED(1), 3}, {literal::REMOVED(3), 1}})
              .equals<i64>({2, 2, 2}));
    CHECK(testing::test_graph(
              deduplicated,
              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>(
                  {{1, 2}, {literal::REMOVED(1), 3}, {literal::REMOVED(3), 1}}))
              .equals<i64>({2, std::nullopt, std::nullopt}));
}

TEST_CASE("len_ and contains_ query scalar time-series lists",
          "[feature:operator][series:value][scalar:list][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<List<i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<List<i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<List<i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(testing::test_graph(length, {{1, 2, 3}, {}}).equals<i64>({3, 0}));
    CHECK(testing::test_graph(contains, {{1, 2, 3}, {1, 3}, {}}).equals<bool>({true, false, false}));
    CHECK(testing::test_graph(contains_key_series, {{1, 2, 3}, {1, 3}, {}}, {2, 3, 1})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query scalar time-series sets",
          "[feature:operator][series:value][scalar:set][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<Set<i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<Set<i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<Set<i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(testing::test_graph(length, {{1, 2, 3}, {2, 3}}).equals<i64>({3, 2}));
    CHECK(testing::test_graph(contains, {{1, 2, 3}, {1, 3}, {}}).equals<bool>({true, false, false}));
    CHECK(testing::test_graph(contains_key_series, {{1, 2, 3}, {1, 3}, {}}, {2, 3, 1})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query scalar time-series dictionaries",
          "[feature:operator][series:value][scalar:dict][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<Dict<i64, i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<Dict<i64, i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<Dict<i64, i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(testing::test_graph(length, {{{1, 10}, {2, 20}}, {{2, 20}}}).equals<i64>({2, 1}));
    CHECK(testing::test_graph(contains, {{{1, 10}, {2, 20}}, {{1, 10}}, {}}).equals<bool>({true, false, false}));
    CHECK(testing::test_graph(contains_key_series, {{{1, 10}, {2, 20}}, {{1, 10}}, {}}, {2, 1, 3})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query strings and symbols without copying text",
          "[feature:operator][series:value][scalar:string][scalar:symbol][shape:single-node]") {
    const auto string_length = [](wp<TS<string>> value) { return operators::len_("value"_n = value); };
    const auto symbol_length = [](wp<TS<symbol>> value) { return operators::len_("value"_n = value); };
    const auto string_contains = [](wp<TS<string>> value, wp<TS<string>> item) {
        return operators::contains_("collection"_n = value, "key"_n = item);
    };
    const auto symbol_contains = [](wp<TS<symbol>> value, wp<TS<symbol>> item) {
        return operators::contains_("collection"_n = value, "key"_n = item);
    };
    const auto strings = std::array{std::string_view{"a\0bc", 4}, std::string_view{"abc"}};
    const auto string_items = std::array{std::string_view{"\0b", 2}, std::string_view{"d"}};
    const auto symbols = std::array{symbol{"alphabet"}, symbol{"gamma"}};
    const auto symbol_items = std::array{symbol{"pha"}, symbol{"am"}};

    CHECK(testing::test_graph(string_length, strings).equals<i64>({4, 3}));
    CHECK(testing::test_graph(symbol_length, symbols).equals<i64>({8, 5}));
    CHECK(testing::test_graph(string_contains, strings, string_items).equals<bool>({true, false}));
    CHECK(testing::test_graph(symbol_contains, symbols, symbol_items).equals<bool>({true, true}));
}
