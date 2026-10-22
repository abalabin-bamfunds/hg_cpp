#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace bg;
using namespace bg::literals;

TEST_CASE("convert copies scalar lists into TSL and TSS snapshots",
          "[feature:operator][series:collection][scalar:collection]") {
    const auto list_to_tsl_sum = [] {
        const auto values = ScalarValue<List<i64>>{std::array<i64, 3>{1, 2, 3}};
        const auto source = operators::const_(values);
        const auto snapshot = operators::convert[type<TSL<TS<i64, TimeSeriesGuarantee::const_>, 3,
                                                         TimeSeriesGuarantee::const_>>](source);
        return operators::sum_(snapshot);
    };

    const auto list_to_tss_sum = [] {
        const auto values = ScalarValue<List<i64>>{std::array<i64, 3>{2, 4, 6}};
        const auto source = operators::const_(values);
        const auto snapshot = operators::convert[type<TSS<i64, TimeSeriesGuarantee::const_>>](source);
        return operators::sum_(snapshot);
    };

    CHECK(testing::test_graph(list_to_tsl_sum).equals<i64>({6}));
    CHECK(testing::test_graph(list_to_tss_sum).equals<i64>({12}));
}

TEST_CASE("convert copies scalar sets into TSS snapshots",
          "[feature:operator][series:collection][scalar:collection]") {
    const auto set_to_tss_sum = [] {
        const auto values = ScalarValue<Set<i64>>{std::array<i64, 3>{2, 4, 6}};
        const auto source = operators::const_(values);
        const auto snapshot = operators::convert[type<TSS<i64, TimeSeriesGuarantee::const_>>](source);
        return operators::sum_(snapshot);
    };

    CHECK(testing::test_graph(set_to_tss_sum).equals<i64>({12}));
}

TEST_CASE("convert reconciles repeated set snapshots without publishing false changes",
          "[feature:operator][feature:collection-change][series:tss][scalar:set]") {
    const auto observe_changes = node([](In<TSS<i64>> values) {
        auto changes = i64{};
        values.for_each_added([&](const auto &) { changes += 10; });
        values.for_each_removed([&](const auto &) { ++changes; });
        return changes;
    });
    const auto convert_and_observe = [&](wp<TS<Set<i64>>> values) {
        return observe_changes(operators::convert[type<TSS<i64>>](values));
    };

    CHECK(testing::test_graph(convert_and_observe,
                              testing::input<TS<Set<i64>>>({{1, 2}, {1, 2}, {2, 3}}))
              .equals<i64>({20, 0, 11}));
}

TEST_CASE("combine captures homogeneous time-series arguments into a TSL",
          "[feature:operator][series:fixed-tsl][scalar:integer]") {
    const auto combine_and_sum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = operators::combine[type<TSL<TS<i64>, 2>>](first, second);
        return operators::sum_(values);
    };

    CHECK(testing::test_graph(combine_and_sum, {1, 10}, {2, 20}).equals<i64>({3, 30}));
}
