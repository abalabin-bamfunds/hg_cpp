#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

TEST_CASE("a deduplicated value output schedules subscribers only for changed values",
          "[evaluation][feature:scheduling][transition:update][series:value][scalar:integer]"
          "[guarantee:deduplicated]") {
    const auto deduplicate =
        bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>> output) {
            output.set(*input);
        });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return deduplicate(input); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 1, 2, 2, 1});

    CHECK(actual.equals({1, std::nullopt, 2, std::nullopt, 1}));
}

TEST_CASE("a collection guarantee deduplicates contained value outputs",
          "[evaluation][feature:collection-change][transition:update][series:fixed-tsl]"
          "[scalar:integer][guarantee:ordinary][guarantee:deduplicated]") {

    const auto collect = bg::node(
        [](bg::In<bg::TS<bg::i64>> input,
           bg::Out<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>, 1, bg::TimeSeriesGuarantee::deduplicated>>
               output) { output[0].set(*input); });
    const auto first = bg::node(
        [](bg::In<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>, 1, bg::TimeSeriesGuarantee::deduplicated>>
               input,
           bg::Out<bg::TS<bg::i64>> output) { output.set(*input[0]); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return first(collect(input)); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("symbol time series can store default and interned values",
          "[feature:test-graph][series:value][scalar:symbol][shape:single-node]") {
    const auto pass_through = bg::node([](bg::In<bg::TS<bg::symbol>> input, bg::Out<bg::TS<bg::symbol>> output) {
        output.set(*input);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::symbol>> input) { return pass_through(input); };
    const auto input = std::array{bg::symbol{}, bg::symbol{"price"}, bg::symbol{""}};

    const auto actual = bg::testing::test_graph(graph, input);

    CHECK(actual.equals({bg::symbol{}, bg::symbol{"price"}, bg::symbol{""}}));
}
