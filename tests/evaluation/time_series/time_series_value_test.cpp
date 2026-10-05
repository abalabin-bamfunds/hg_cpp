#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <vector>

namespace {

inline constexpr auto G = bg::guarantee_variable<"G">;

struct deduplicating_value_source {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>> output;

    void evaluate() const { output.set(*input); }
};

struct preserve_value_guarantee {
    bg::In<bg::TS<bg::i64, G>> input;
    bg::In<bg::TS<bg::i64>> trigger;
    bg::Out<bg::TS<bg::i64, G>> output;

    void evaluate() const {
        static_cast<void>(trigger);
        output.set(*input);
    }
};

struct make_dictionary_with_guarantee {
    bg::In<bg::TS<bg::i64, G>> input;
    bg::In<bg::TS<bg::i64>> trigger;
    bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64, G>>> output;

    void evaluate() const {
        static_cast<void>(trigger);
        output[1].set(*input);
    }
};

struct validity_transition_sink {
    bg::In<bg::TS<bg::i64>, bg::optional> input;

    void evaluate() const {
        states.push_back({input.valid(), input.became_valid(), input.became_invalid(), input.modified()});
        times.push_back(input.last_updated());
    }

    inline static std::vector<std::array<bool, 4>> states;
    inline static std::vector<bg::EvaluationTime> times;
};

} // namespace

TEST_CASE("an optional input exposes its latest validity transition",
          "[evaluation][series:value][transition:first-tick][transition:update]"
          "[transition:invalidation][transition:revalidation][feature:validity][validity:optional]") {
    const auto source = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (*input == 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto sink = bg::node<validity_transition_sink>();
    validity_transition_sink::states.clear();
    validity_transition_sink::times.clear();

    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        const auto output = source(input);
        sink(output);
        return output;
    };
    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 4>{1, 2, 0, 3});

    CHECK(actual.equals({1, 2, std::nullopt, 3}));
    CHECK(validity_transition_sink::states == std::vector<std::array<bool, 4>>{
                                                {true, true, false, true},
                                                {true, false, false, true},
                                                {false, false, true, false},
                                                {true, true, false, true},
                                            });
    REQUIRE(validity_transition_sink::times.size() == 4);
    CHECK(validity_transition_sink::times[0] < validity_transition_sink::times[1]);
    CHECK(validity_transition_sink::times[1] < validity_transition_sink::times[2]);
    CHECK(validity_transition_sink::times[2] < validity_transition_sink::times[3]);
}

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

TEST_CASE("a direct output uses its resolved guarantee",
          "[evaluation][series:value][guarantee:deduplicated][type_system]") {
    const auto deduplicate = bg::node<deduplicating_value_source>();
    const auto preserve_guarantee = bg::node<preserve_value_guarantee>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return preserve_guarantee(deduplicate(input), input);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("a nested direct output uses its resolved item guarantee",
          "[evaluation][series:dictionary][guarantee:deduplicated][type_system]") {
    const auto deduplicate = bg::node<deduplicating_value_source>();
    const auto make_dictionary = bg::node<make_dictionary_with_guarantee>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return make_dictionary(deduplicate(input), input)[1].as<
            bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>>();
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
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
