#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

using namespace bg;
using namespace bg::literals;

TEST_CASE("fixed TSL getitem changes selection with a time series index",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]") {
    const auto graph = [](wp<TSL<TS<i64>, 2>> input, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
        return input[index].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(graph, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}, {{0, 3}, {1, 30}}},
                                            {0, 1, 0});

    CHECK(actual.equals({1, 20, 3}));
}

TEST_CASE("fixed TSL getitem observes only the selected item",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:free][binding:value-to-reference][binding:reference-to-value]") {
    const auto graph = [](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
        return tsl(first, second)[index].as<TS<i64>>();
    };
    const auto first = std::array{1, 2, 3};
    const auto second = std::array{std::optional{10}, std::optional<int>{}, std::optional{30}};
    const auto index =
        std::array{std::optional<std::int64_t>{1}, std::optional<std::int64_t>{}, std::optional<std::int64_t>{}};

    const auto actual = testing::test_graph(graph, first, second, index);

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30}));
}

TEST_CASE("fixed TSL getitem invalidates for an invalid or out-of-range time series index",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSL<TS<i64>, 2>> input, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
        return input[index].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        graph, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}, {{0, 3}, {1, 30}}, {{0, 4}, {1, 40}}},
        {0, literal::INVALID, 2, 1});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, std::nullopt, 40}));
}

TEST_CASE("fixed TSL getitem selects stored references with a time series index",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][feature:operator]"
          "[representation:native][binding:reference-to-reference][binding:reference-to-value]") {
    const auto increment = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input + 1); });
    const auto scale = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input * 10); });
    const auto make_list = node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second, Out<TSL<REF<TS<i64>>, 2>> output) {
        output[0].copy(first);
        output[1].copy(second);
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
        return make_list(increment(input), scale(input))[index].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, std::array<std::int64_t, 3>{0, 1, 0});

    CHECK(actual.equals({2, 20, 4}));
}

TEST_CASE("fixed TSL getitem forwards updates from a selected stored reference",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][feature:reference]"
          "[representation:native][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:update]") {
    const auto increment = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input + 1); });
    const auto make_list = node([](In<REF<TS<i64>>> item, Out<TSL<REF<TS<i64>>, 2>> output) {
        output[0].copy(item);
        output[1].copy(item);
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
        return make_list(increment(input))[index].as<TS<i64>>();
    };
    const auto index =
        std::array{std::optional<std::int64_t>{0}, std::optional<std::int64_t>{}, std::optional<std::int64_t>{}};

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, index);

    CHECK(actual.equals({2, 3, 4}));
}

TEST_CASE("fixed TSL getitem follows selected item invalidation and revalidation",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:reference]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSL<TS<i64>, 2>> input, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
        return input[index].as<TS<i64>>();
    };
    const auto actual = testing::test_graph(
        graph, {{{0, 1}, {1, 10}}, {{0, literal::INVALID}, {1, 20}}, {{0, 2}, {1, 30}}},
        {0, literal::SKIP, literal::SKIP});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, 2}));
}

TEST_CASE("fixed TSL getitem follows a list across a nested graph boundary",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:nested-graph]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto make_list = node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        output[0].set(*input);
        output[1].set(*input * 10);
    });
    const auto inner = graph("name"_n = "make_list", [&](wp<TS<i64>> input) { return make_list(input); });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<std::int64_t>> index, wp<TS<bool>> run) -> wp<TS<i64>> {
        return nested(inner, run, input)[index].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(outer, {1, 2, 3, 4}, {0, 1, 0, 1}, {true, false, true, true});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, 3, 40}));
}

TEST_CASE("scalar getitem reads a reference-backed fixed list",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:nested-graph]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value][shape:nested]") {
    const auto make_list = node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        output[0].set(*input);
        output[1].set(*input * 10);
    });
    const auto inner = graph("name"_n = "make_list", [&](wp<TS<i64>> input) { return make_list(input); });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) -> wp<TS<i64>> {
        return nested(inner, run, input)[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(outer, {1, 2, 3}, {true, true, true});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("fixed TSL getitem follows a free list into a nested graph",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:nested-graph]"
          "[representation:free][binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto inner =
        graph("name"_n = "select_list_item", [](wp<TSL<TS<i64>, 2>> input, wp<TS<std::int64_t>> index) -> wp<TS<i64>> {
            return input[index].as<TS<i64>>();
        });
    const auto outer = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<std::int64_t>> index, wp<TS<bool>> run) {
        return nested(inner, run, tsl(first, second), index);
    };

    const auto actual =
        testing::test_graph(outer, std::array{1, 2, 3, 4}, std::array{10, 20, 30, 40},
                            std::array<std::int64_t, 4>{0, 1, 0, 1}, std::array{true, false, true, true});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, 3, 40}));
}
