#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

TEST_CASE("a deduplicated value output schedules subscribers only for changed values",
          "[evaluation][feature:scheduling][transition:update][series:value][scalar:integer]"
          "[guarantee:deduplicated]") {
    const auto deduplicate =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int, bg::TimeSeriesGuarantee::deduplicated>> output) {
            output.set(*input);
        });
    const auto graph = [&](bg::wp<bg::TS<int>> input) { return deduplicate(input); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 1, 2, 2, 1});

    CHECK(actual.equals({1, std::nullopt, 2, std::nullopt, 1}));
}

TEST_CASE("a collection guarantee deduplicates contained value outputs",
          "[evaluation][feature:collection-change][transition:update][series:fixed-tsl]"
          "[scalar:integer][guarantee:ordinary][guarantee:deduplicated]") {
    using Values = bg::TSL<bg::TS<int, bg::TimeSeriesGuarantee::ordinary>, 1, bg::TimeSeriesGuarantee::deduplicated>;

    const auto collect = bg::node([](bg::In<bg::TS<int>> input, bg::Out<Values> output) { output[0].set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input) { return collect(input)[0]; };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}
