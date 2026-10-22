#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <vector>

using namespace bg;

TEST_CASE("symbol time series can store default and interned values",
          "[feature:test-graph][series:value][scalar:symbol][shape:single-node]") {
    const auto pass_through = node([](symbol input) { return input; });

    const auto graph = [&](wp<TS<symbol>> input) { return pass_through(input); };

    const auto input = std::array{symbol{}, symbol{"price"}, symbol{""}};

    const auto actual = testing::test_graph(graph, input);

    CHECK(actual.equals({symbol{}, symbol{"price"}, symbol{""}}));
}

namespace {

inline constexpr auto G = guarantee_variable<"G">;

struct deduplicating_value_source {
    In<TS<i64>> input;
    Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output;

    void evaluate() const { output.set(*input); }
};

struct preserve_value_guarantee {
    In<TS<i64, G>> input;
    In<TS<i64>> trigger;
    Out<TS<i64, G>> output;

    void evaluate() const {
        static_cast<void>(trigger);
        output.set(*input);
    }
};

struct make_dictionary_with_guarantee {
    In<TS<i64, G>> input;
    In<TS<i64>> trigger;
    Out<TSD<i64, TS<i64, G>>> output;

    void evaluate() const {
        static_cast<void>(trigger);
        output[1].set(*input);
    }
};

} // namespace

TEST_CASE("an optional input exposes its latest validity transition",
          "[evaluation][series:value][transition:first-tick][transition:update]"
          "[transition:invalidation][transition:revalidation][feature:validity][validity:optional]") {
    auto states = std::vector<std::array<bool, 4>>{};
    auto times = std::vector<EvaluationTime>{};

    const auto source = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });

    const auto observe = node([&](In<TS<i64>, optional> input) {
        states.push_back({input.valid(), input.became_valid(), input.became_invalid(), input.modified()});
        times.push_back(input.last_updated());
    });

    const auto graph = [&](wp<TS<i64>> input) {
        const auto output = source(input);
        observe(output);
        return output;
    };

    const auto actual = testing::test_graph(graph, std::array<i64, 4>{1, 2, 0, 3});

    CHECK(actual.equals({1, 2, std::nullopt, 3}));
    CHECK(states == std::vector<std::array<bool, 4>>{
                        {true, true, false, true},
                        {true, false, false, true},
                        {false, false, true, true},
                        {true, true, false, true},
                    });
    REQUIRE(times.size() == 4);
    CHECK(times[0] < times[1]);
    CHECK(times[1] < times[2]);
    CHECK(times[2] < times[3]);
}

TEST_CASE("input changes and validity transitions apply only to the current evaluation",
          "[evaluation][feature:validity][validity:optional][transition:invalidation]"
          "[series:value][scalar:integer][shape:chain]") {
    struct Observation {
        i64 trigger;
        bool valid;
        bool became_valid;
        bool became_invalid;
        bool modified;

        bool operator==(const Observation &) const = default;
    };

    const auto observe = node([](In<TS<i64>, passive, optional> input,
                                     In<TS<i64>> trigger) {
        return Observation{
            .trigger = *trigger,
            .valid = input.valid(),
            .became_valid = input.became_valid(),
            .became_invalid = input.became_invalid(),
            .modified = input.modified(),
        };
    });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> trigger) {
        return observe(input, trigger);
    };

    const auto actual = testing::test_graph(
        graph, {1, 1, literal::INVALID, literal::SKIP, 2, literal::SKIP}, {1, 2, 3, 4, 5, 6});

    CHECK(actual.equals<Observation>({
        Observation{1, true, true, false, true},
        Observation{2, true, false, false, true},
        Observation{3, false, false, true, true},
        Observation{4, false, false, false, false},
        Observation{5, true, true, false, true},
        Observation{6, true, false, false, false},
    }));
}

TEST_CASE("a deduplicated value output schedules subscribers only for changed values",
          "[evaluation][feature:scheduling][transition:update][series:value][scalar:integer]"
          "[guarantee:deduplicated]") {
    const auto deduplicate =
        node([](In<TS<i64>> input, Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) {
            output.set(*input);
        });

    const auto graph = [&](wp<TS<i64>> input) { return deduplicate(input); };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2, 2, 1});

    CHECK(actual.equals({1, std::nullopt, 2, std::nullopt, 1}));
}

TEST_CASE("a direct output uses its resolved guarantee",
          "[evaluation][feature:type-system][series:value][guarantee:deduplicated]") {
    const auto deduplicate = node<deduplicating_value_source>();

    const auto preserve_guarantee = node<preserve_value_guarantee>();

    const auto graph = [&](wp<TS<i64>> input) {
        return preserve_guarantee(deduplicate(input), input);
    };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("a nested direct output uses its resolved item guarantee",
          "[evaluation][feature:type-system][series:tsd][guarantee:deduplicated]") {
    const auto deduplicate = node<deduplicating_value_source>();

    const auto make_dictionary = node<make_dictionary_with_guarantee>();

    const auto graph = [&](wp<TS<i64>> input) {
        return make_dictionary(deduplicate(input), input)[1];
    };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals<i64>({1, std::nullopt, 2}));
}

TEST_CASE("a collection guarantee deduplicates contained value outputs",
          "[evaluation][feature:collection-change][transition:update][series:fixed-tsl]"
          "[scalar:integer][guarantee:ordinary][guarantee:deduplicated]") {

    const auto collect = node(
        [](In<TS<i64>> input,
           Out<TSL<TS<i64, TimeSeriesGuarantee::ordinary>, 1, TimeSeriesGuarantee::deduplicated>>
               output) { output[0].set(*input); });

    const auto first = node(
        [](In<TSL<TS<i64, TimeSeriesGuarantee::ordinary>, 1, TimeSeriesGuarantee::deduplicated>>
               input) { return *input[0]; });

    const auto graph = [&](wp<TS<i64>> input) { return first(collect(input)); };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}
