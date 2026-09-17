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
    const auto make_list = node([](In<TS<int>> input, Out<TSL<TS<int>, 2>> output) {
        output[0].set(*input);
        output[1].set(*input * 10);
    });
    const auto graph = [&](wp<TS<int>> input, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
        return make_list(input)[index].as<TS<int>>();
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, std::array<std::int64_t, 3>{0, 1, 0});

    CHECK(actual.equals({1, 20, 3}));
}

TEST_CASE("fixed TSL getitem observes only the selected item",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:free][binding:value-to-reference][binding:reference-to-value]") {
    const auto graph = [](wp<TS<int>> first, wp<TS<int>> second, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
        return tsl(first, second)[index].as<TS<int>>();
    };
    const auto first = std::array{1, 2, 3};
    const auto second = std::array{std::optional{10}, std::optional<int>{}, std::optional{30}};
    const auto index =
        std::array{std::optional<std::int64_t>{1}, std::optional<std::int64_t>{}, std::optional<std::int64_t>{}};

    const auto actual = testing::test_graph(graph, first, second, index);

    CHECK(actual.equals<int>({10, std::nullopt, 30}));
}

TEST_CASE("fixed TSL getitem invalidates for an invalid or out-of-range time series index",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto make_list = node([](In<TS<int>> input, Out<TSL<TS<int>, 2>> output) {
        output[0].set(*input);
        output[1].set(*input * 10);
    });
    const auto make_index = node([](In<TS<std::int64_t>> input, Out<TS<std::int64_t>> output) {
        if (*input < 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto graph = [&](wp<TS<int>> input, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
        return make_list(input)[make_index(index)].as<TS<int>>();
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3, 4}, std::array<std::int64_t, 4>{0, -1, 2, 1});

    CHECK(actual.equals<int>({1, std::nullopt, std::nullopt, 40}));
}

TEST_CASE("fixed TSL getitem selects stored references with a time series index",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][feature:operator]"
          "[representation:native][binding:reference-to-reference][binding:reference-to-value]") {
    const auto increment = node([](In<TS<int>> input, Out<TS<int>> output) { output.set(*input + 1); });
    const auto scale = node([](In<TS<int>> input, Out<TS<int>> output) { output.set(*input * 10); });
    const auto make_list = node([](In<REF<TS<int>>> first, In<REF<TS<int>>> second, Out<TSL<REF<TS<int>>, 2>> output) {
        output[0].copy(first);
        output[1].copy(second);
    });
    const auto graph = [&](wp<TS<int>> input, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
        return make_list(increment(input), scale(input))[index].as<TS<int>>();
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, std::array<std::int64_t, 3>{0, 1, 0});

    CHECK(actual.equals({2, 20, 4}));
}

TEST_CASE("fixed TSL getitem forwards updates from a selected stored reference",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][feature:reference]"
          "[representation:native][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:update]") {
    const auto increment = node([](In<TS<int>> input, Out<TS<int>> output) { output.set(*input + 1); });
    const auto make_list = node([](In<REF<TS<int>>> item, Out<TSL<REF<TS<int>>, 2>> output) {
        output[0].copy(item);
        output[1].copy(item);
    });
    const auto graph = [&](wp<TS<int>> input, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
        return make_list(increment(input))[index].as<TS<int>>();
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
    const auto make_list = node([](In<TS<int>> input, Out<TSL<TS<int>, 2>> output) {
        if (*input < 0) {
            output[0].invalidate();
        } else {
            output[0].set(*input);
        }
        output[1].set(*input * 10);
    });
    const auto graph = [&](wp<TS<int>> input, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
        return make_list(input)[index].as<TS<int>>();
    };
    const auto index =
        std::array{std::optional<std::int64_t>{0}, std::optional<std::int64_t>{}, std::optional<std::int64_t>{}};

    const auto actual = testing::test_graph(graph, std::array{1, -1, 2}, index);

    CHECK(actual.equals<int>({1, std::nullopt, 2}));
}

TEST_CASE("fixed TSL getitem follows a list across a nested graph boundary",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:nested-graph]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto make_list = node([](In<TS<int>> input, Out<TSL<TS<int>, 2>> output) {
        output[0].set(*input);
        output[1].set(*input * 10);
    });
    const auto inner = graph("name"_n = "make_list", [&](wp<TS<int>> input) { return make_list(input); });
    const auto outer = [&](wp<TS<int>> input, wp<TS<std::int64_t>> index, wp<TS<bool>> run) -> wp<TS<int>> {
        return nested(inner, run, input)[index].as<TS<int>>();
    };

    const auto actual = testing::test_graph(outer, std::array{1, 2, 3, 4}, std::array<std::int64_t, 4>{0, 1, 0, 1},
                                            std::array{true, false, true, true});

    CHECK(actual.equals<int>({1, std::nullopt, 3, 40}));
}

TEST_CASE("fixed TSL getitem follows a free list into a nested graph",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:nested-graph]"
          "[representation:free][binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto inner =
        graph("name"_n = "select_list_item", [](wp<TSL<TS<int>, 2>> input, wp<TS<std::int64_t>> index) -> wp<TS<int>> {
            return input[index].as<TS<int>>();
        });
    const auto outer = [&](wp<TS<int>> first, wp<TS<int>> second, wp<TS<std::int64_t>> index, wp<TS<bool>> run) {
        return nested(inner, run, tsl(first, second), index);
    };

    const auto actual =
        testing::test_graph(outer, std::array{1, 2, 3, 4}, std::array{10, 20, 30, 40},
                            std::array<std::int64_t, 4>{0, 1, 0, 1}, std::array{true, false, true, true});

    CHECK(actual.equals<int>({1, std::nullopt, 3, 40}));
}
