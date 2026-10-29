#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>
#include <bg/switch.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>

using namespace bg;
using namespace bg::literals;

TEST_CASE("fixed TSL getitem changes selection with a time series index",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]") {
    const auto graph = [](wp<TSL<TS<i64>, 2>> input, wp<TS<i64>> index) {
        return input[index];
    };

    const auto actual = bg::run_graph(graph, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}, {{0, 3}, {1, 30}}},
                                            {0, 1, 0});

    CHECK(actual.equals<i64>({1, 20, 3}));
}

TEST_CASE("fixed TSL getitem observes only the selected item",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:free][binding:value-to-reference][binding:reference-to-value]") {
    const auto graph = [](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> index) {
        return tsl(first, second)[index];
    };
    const auto first = std::array{1, 2, 3};
    const auto second = std::array{std::optional{10}, std::optional<int>{}, std::optional{30}};
    const auto index =
        std::array{std::optional<i64>{1}, std::optional<i64>{}, std::optional<i64>{}};

    const auto actual = bg::run_graph(graph, first, second, index);

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30}));
}

TEST_CASE("fixed TSL getitem tracks target identity across references to the same target",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][guarantee:deduplicated]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value]") {
    struct Observation {
        i64 value;
        bool modified;
        EvaluationTime last_updated;

        bool operator==(const Observation &) const = default;
    };

    const auto select = node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                Out<REF<TS<i64>, TimeSeriesGuarantee::deduplicated>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto observe = node([](In<TS<i64>> input, In<TS<i64>>, Out<TS<Observation>> output) {
        output.set(Observation{*input, input.modified(), input.last_updated()});
    });

    const auto graph = [&](wp<TS<i64>> value, wp<TS<i64>> other, wp<TS<i64>> first_index,
                           wp<TS<i64>> second_index, wp<TS<bool>> use_second, wp<TS<i64>> trigger) {
        const auto first = operators::getitem(tsl(value, other), first_index);
        const auto second = operators::getitem(tsl(other, value), second_index);
        return observe(select(use_second, first, second), trigger);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluate = [&](std::array<bool, 5> route) {
        return bg::run_graph(
            graph, GraphRunOptions{.start_time = start_time},
            std::array<std::optional<i64>, 5>{10, std::nullopt, std::nullopt, 20, 20},
            std::array<i64, 5>{-1, -2, -3, -4, -5}, std::array<i64, 5>{0, 0, 0, 0, 0},
            std::array<i64, 5>{1, 1, 1, 1, 1}, route, std::array<i64, 5>{1, 2, 3, 4, 5});
    };

    const auto expected = std::initializer_list<std::optional<Observation>>{
        Observation{10, true, start_time},
        Observation{10, false, start_time},
        Observation{10, false, start_time},
        Observation{20, true, start_time + std::chrono::microseconds{3}},
        Observation{20, true, start_time + std::chrono::microseconds{4}},
    };
    CHECK(evaluate({false, true, false, true, false}).equals<Observation>(expected));
    CHECK(evaluate({true, false, true, false, true}).equals<Observation>(expected));

    const auto distinct_sources = bg::run_graph(
        graph, GraphRunOptions{.start_time = start_time},
        std::array<std::optional<i64>, 5>{10, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{10, std::nullopt, std::nullopt, std::nullopt, 50},
        std::array<i64, 5>{0, 0, 0, 0, 0}, std::array<i64, 5>{1, 1, 0, 0, 0},
        std::array<bool, 5>{false, true, true, true, true}, std::array<i64, 5>{1, 2, 3, 4, 5});

    CHECK(distinct_sources.equals<Observation>({
        Observation{10, true, start_time},
        Observation{10, false, start_time},
        Observation{10, true, start_time + std::chrono::microseconds{2}},
        Observation{10, false, start_time + std::chrono::microseconds{2}},
        Observation{50, true, start_time + std::chrono::microseconds{4}},
    }));
}

TEST_CASE("switching between pass-through and rebuilt free lists preserves item target identity",
          "[evaluation][feature:switch][feature:reference][feature:sampling][series:fixed-tsl]"
          "[representation:free][shape:nested][binding:reference-to-reference][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_free_list",
                                    [](wp<TSL<TS<i64>, 2>> input) { return input; });

    const auto rebuild = graph("name"_n = "rebuild_free_list", [](wp<TSL<TS<i64>, 2>> input) {
        return tsl(input[0], input[1]);
    });

    const auto first_value = node([](i64 first) { return first; });

    const auto select_first = graph("name"_n = "select_free_list_first", [&](wp<TSL<TS<i64>, 2>> input) {
        return first_value(input[0]);
    });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> first, wp<TS<i64>> second) {
        return select_first(switch_(key, cases{case_(true, pass_through), case_(false, rebuild)},
                                    tsl(first, second)));
    };

    const auto starts_with_pass_through =
        bg::run_graph(tested, {true, false, true, false}, {10, literal::SKIP, literal::SKIP, 20},
                      {-1, -2, -3, -4});
    const auto starts_with_rebuilt =
        bg::run_graph(tested, {false, true, false, true}, {10, literal::SKIP, literal::SKIP, 20},
                      {-1, -2, -3, -4});

    CHECK(starts_with_pass_through.equals<i64>({10, std::nullopt, std::nullopt, 20}));
    CHECK(starts_with_rebuilt.equals<i64>({10, std::nullopt, std::nullopt, 20}));
}

TEST_CASE("fixed TSL getitem invalidates for an invalid or out-of-range time series index",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:operator]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSL<TS<i64>, 2>> input, wp<TS<i64>> index) {
        return input[index];
    };

    const auto actual = bg::run_graph(
        graph, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}, {{0, 3}, {1, 30}}, {{0, 4}, {1, 40}}},
        {0, literal::INVALID, 2, 1});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, std::nullopt, 40}));
}

TEST_CASE("fixed TSL getitem selects stored references with a time series index",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][feature:operator]"
          "[representation:native][binding:reference-to-reference][binding:reference-to-value]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto scale = node([](i64 input) { return input * 10; });

    const auto make_list = node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second, Out<TSL<REF<TS<i64>>, 2>> output) {
        output[0].copy(first);
        output[1].copy(second);
    });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> index) {
        return make_list(increment(input), scale(input))[index];
    };

    const auto actual = bg::run_graph(graph, std::array{1, 2, 3}, std::array<i64, 3>{0, 1, 0});

    CHECK(actual.equals<i64>({2, 20, 4}));
}

TEST_CASE("fixed TSL getitem forwards updates from a selected stored reference",
          "[evaluation][series:fixed-tsl][series:reference][feature:getitem][feature:reference]"
          "[representation:native][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:update]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto make_list = node([](In<REF<TS<i64>>> item, Out<TSL<REF<TS<i64>>, 2>> output) {
        output[0].copy(item);
        output[1].copy(item);
    });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> index) {
        return make_list(increment(input))[index];
    };
    const auto index =
        std::array{std::optional<i64>{0}, std::optional<i64>{}, std::optional<i64>{}};

    const auto actual = bg::run_graph(graph, std::array{1, 2, 3}, index);

    CHECK(actual.equals<i64>({2, 3, 4}));
}

TEST_CASE("fixed TSL getitem follows selected item invalidation and revalidation",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:reference]"
          "[representation:native][binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSL<TS<i64>, 2>> input, wp<TS<i64>> index) {
        return input[index];
    };

    const auto actual = bg::run_graph(
        graph, {{{0, 1}, {1, 10}}, {{0, literal::INVALID}, {1, -10}}, {{0, 2}, {1, 20}}},
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

    const auto outer = [&](wp<TS<i64>> input, wp<TS<i64>> index, wp<TS<bool>> run) {
        return nested(inner, run, input)[index];
    };

    const auto actual = bg::run_graph(outer, {1, 2, 3, 4}, {0, 1, 0, 1}, {true, false, true, true});

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

    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) {
        return nested(inner, run, input)[i64{1}];
    };

    const auto actual = bg::run_graph(outer, {1, 2, 3}, {true, true, true});

    CHECK(actual.equals<i64>({10, 20, 30}));
}

TEST_CASE("fixed TSL getitem follows a free list into a nested graph",
          "[evaluation][series:fixed-tsl][series:value][feature:getitem][feature:nested-graph]"
          "[representation:free][binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto inner =
        graph("name"_n = "select_list_item", [](wp<TSL<TS<i64>, 2>> input, wp<TS<i64>> index) -> wp<TS<i64>> {
            return input[index].as<TS<i64>>();
        });

    const auto outer = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> index, wp<TS<bool>> run) {
        return nested(inner, run, tsl(first, second), index);
    };

    const auto actual =
        bg::run_graph(outer, std::array{1, 2, 3, 4}, std::array{10, 20, 30, 40},
                            std::array<i64, 4>{0, 1, 0, 1}, std::array{true, false, true, true});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, 3, 40}));
}
