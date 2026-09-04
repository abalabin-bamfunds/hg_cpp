#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>

namespace {

using namespace bg::literals;

struct LifecycleIncrement {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a nested graph can be stopped and started repeatedly",
          "[evaluation][nested][lifecycle][feature:nested-graph][shape:nested][transition:start]"
          "[transition:stop][transition:restart][scalar:boolean][scalar:integer][series:value]") {
    const auto increment = bg::node<LifecycleIncrement>();
    const auto inner =
        bg::graph("name"_m = "inner_increment",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return increment(input); });
    LifecycleIncrement::starts = 0;
    LifecycleIncrement::stops = 0;

    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, input);
    };

    const auto values = {1, 2, 3, 4, 5};
    const auto running = {true, false, true, false, true};
    const auto actual = bg::testing::test_graph(outer, values, running);
    CHECK(actual.equals({2, std::nullopt, 4, std::nullopt, 6}));
    CHECK(LifecycleIncrement::starts == 3);
    CHECK(LifecycleIncrement::stops == 3);
}

struct ReferenceIncrement {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct ReferenceDouble {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input * 2); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a reference connects two nested graphs",
          "[evaluation][nested][reference][feature:nested-graph][shape:nested][scalar:integer]"
          "[series:value][binding:value-to-reference][binding:reference-to-value]") {
    const auto increment = bg::node<ReferenceIncrement>();
    const auto twice = bg::node<ReferenceDouble>();
    const auto first =
        bg::graph("name"_m = "first",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return increment(input); });
    const auto second =
        bg::graph("name"_m = "second",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return twice(input); });
    ReferenceIncrement::starts = 0;
    ReferenceIncrement::stops = 0;
    ReferenceDouble::starts = 0;
    ReferenceDouble::stops = 0;

    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        const auto first_output = bg::nested(first, run, input);
        return bg::nested(second, run, first_output);
    };

    const auto values = {1, 2, 3, 4, 5};
    const auto running = {true, true, false, true, true};
    const auto actual = bg::testing::test_graph(outer, values, running);
    CHECK(actual.equals({4, 6, std::nullopt, 10, 12}));
    CHECK(ReferenceIncrement::starts == 2);
    CHECK(ReferenceIncrement::stops == 2);
    CHECK(ReferenceDouble::starts == 2);
    CHECK(ReferenceDouble::stops == 2);
}

struct NestedLeafIncrement {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct NestedMiddlePassThrough {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a nested graph can own another nested graph",
          "[evaluation][nested][lifecycle][reference][feature:nested-graph][shape:nested]"
          "[shape:doubly-nested][scalar:integer][series:value][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto increment = bg::node<NestedLeafIncrement>();
    const auto pass_through = bg::node<NestedMiddlePassThrough>();
    const auto leaf =
        bg::graph("name"_m = "leaf",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return increment(input); });
    const auto middle =
        bg::graph("name"_m = "middle",
                  [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) -> bg::wp<bg::TS<std::int64_t>> {
                      const auto value = pass_through(input);
                      return bg::wp<bg::TS<std::int64_t>>{bg::nested(leaf, run, value)};
                  });
    NestedLeafIncrement::starts = 0;
    NestedLeafIncrement::stops = 0;
    NestedMiddlePassThrough::starts = 0;
    NestedMiddlePassThrough::stops = 0;

    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(middle, run, input, run);
    };

    const auto values = {1, 2, 3, 4, 5};
    const auto running = {true, false, true, false, true};
    const auto actual = bg::testing::test_graph(outer, values, running);
    CHECK(actual.equals({2, std::nullopt, 4, std::nullopt, 6}));
    CHECK(NestedMiddlePassThrough::starts == 3);
    CHECK(NestedMiddlePassThrough::stops == 3);
    CHECK(NestedLeafIncrement::starts == 3);
    CHECK(NestedLeafIncrement::stops == 3);
}

using NestedIntDictionary = bg::TSD<std::int64_t, bg::TS<std::int64_t>>;
using NestedIntReferenceDictionary = bg::TSD<std::int64_t, bg::REF<bg::TS<std::int64_t>>>;

struct NestedDictionarySource {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<NestedIntDictionary> output;

    void evaluate() const {
        output[1].set(*input);
        output[2].set(*input * 10);
    }
};

struct NestedSelectedCopy {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(*input); }
};

TEST_CASE("an outer dictionary provides references to an inner graph",
          "[evaluation][nested][reference][tsd][series:tsd][feature:nested-graph][feature:getitem]"
          "[shape:nested][boundary][binding:value-to-reference][binding:reference-to-value]") {
    const auto dictionary = bg::node<NestedDictionarySource>();
    const auto copy = bg::node<NestedSelectedCopy>();
    const auto inner = bg::graph(
        "name"_m = "consume_outer_dictionary",
        [&](bg::wp<NestedIntReferenceDictionary> input) -> bg::wp<bg::TS<std::int64_t>> { return copy(input[1]); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, dictionary(input));
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array{true, true, false, true});
    CHECK(actual.equals({1, 2, std::nullopt, 4}));
}

TEST_CASE("an inner dictionary provides item references to its outer graph",
          "[evaluation][nested][reference][tsd][series:tsd][feature:nested-graph][feature:getitem]"
          "[shape:nested][boundary][binding:value-to-reference][binding:reference-to-value]") {
    const auto dictionary = bg::node<NestedDictionarySource>();
    const auto inner = bg::graph(
        "name"_m = "produce_inner_dictionary",
        [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<NestedIntDictionary> { return dictionary(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, input)[1];
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array{true, true, false, true});
    CHECK(actual.equals({1, 2, std::nullopt, 4}));
}

TEST_CASE("a dictionary reference representation connects two nested graphs",
          "[evaluation][nested][reference][tsd][series:tsd][feature:nested-graph][feature:getitem]"
          "[shape:nested][boundary][binding:value-to-reference][binding:reference-to-value]") {
    const auto dictionary = bg::node<NestedDictionarySource>();
    const auto copy = bg::node<NestedSelectedCopy>();
    const auto producer = bg::graph(
        "name"_m = "dictionary_producer",
        [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<NestedIntDictionary> { return dictionary(input); });
    const auto consumer = bg::graph(
        "name"_m = "dictionary_consumer",
        [&](bg::wp<NestedIntReferenceDictionary> input) -> bg::wp<bg::TS<std::int64_t>> { return copy(input[1]); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(consumer, run, bg::nested(producer, run, input));
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array{true, true, false, true});
    CHECK(actual.equals({1, 2, std::nullopt, 4}));
}

TEST_CASE("a doubly nested dictionary reference is removed and restored with its owner",
          "[evaluation][nested][reference][tsd][series:tsd][feature:nested-graph][feature:getitem]"
          "[shape:doubly-nested][boundary][transition:stop][transition:restart]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto dictionary = bg::node<NestedDictionarySource>();
    const auto copy = bg::node<NestedSelectedCopy>();
    const auto leaf = bg::graph(
        "name"_m = "dictionary_leaf",
        [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<NestedIntDictionary> { return dictionary(input); });
    const auto middle = bg::graph(
        "name"_m = "dictionary_middle",
        [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) -> bg::wp<bg::TS<std::int64_t>> {
            return copy(bg::nested(leaf, run, input)[1]);
        });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(middle, run, input, run);
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 5>{1, 2, 3, 4, 5},
                                                std::array{true, false, true, false, true});
    CHECK(actual.equals({1, std::nullopt, 3, std::nullopt, 5}));
}

struct InnerTimer {
    static constexpr bool uses_future_scheduling = true;

    bg::In<bg::TS<std::int64_t>, bg::passive> input;
    bg::State<int> step;
    bg::Out<bg::TS<std::int64_t>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(*input);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        ++*step;
        output.set(*input + *step);
        if (*step != 2) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

TEST_CASE("future inner scheduling selects an outer cycle",
          "[evaluation][nested][scheduling][feature:nested-graph][shape:nested][scalar:integer]"
          "[series:value]") {
    const auto timer = bg::node<InnerTimer>();
    const auto inner =
        bg::graph("name"_m = "inner_timer",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return timer(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, input);
    };

    const auto values = std::array<std::int64_t, 1>{10};
    const auto running = std::array{true};
    const auto actual = bg::testing::test_graph(outer, values, running);
    CHECK(actual.equals({10, 11, 12}));
}

struct NestedFreeListSum {
    bg::In<bg::TSL<bg::TS<std::int64_t>, 2>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

TEST_CASE("a free list processes reference changes in a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free]"
          "[feature:nested-graph][shape:nested][transition:update][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto sum = bg::node<NestedFreeListSum>();
    const auto inner = bg::graph(
        "name"_m = "inner_free_list",
        [&](bg::wp<bg::TS<std::int64_t>> first,
            bg::wp<bg::TS<std::int64_t>> second) -> bg::wp<bg::TS<std::int64_t>> {
            return sum(bg::tsl(first, second));
        });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return bg::nested(inner, run, first, second); };
    const auto first = std::array<std::int64_t, 3>{1, 2, 3};
    const auto second = std::array<std::int64_t, 3>{10, 20, 30};
    const auto running = std::array{true, true, true};

    const auto actual = bg::testing::test_graph(outer, first, second, running);

    CHECK(actual.equals({11, 22, 33}));
}

using NestedIntList = bg::TSL<bg::TS<std::int64_t>, 2>;

struct NestedListSum {
    bg::In<NestedIntList> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

TEST_CASE("a reference to an outer free list works in a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedListSum>();
    const auto inner = bg::graph(
        "name"_m = "consume_outer_free_list",
        [&](bg::wp<NestedIntList> input) -> bg::wp<bg::TS<std::int64_t>> { return sum(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return bg::nested(inner, run, bg::tsl(first, second)); };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array<std::int64_t, 4>{10, 20, 30, 40},
                                                std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a reference to an inner free list works in the outer graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedListSum>();
    const auto materialize = bg::graph(
        "name"_m = "materialize_free_list",
        [](bg::wp<NestedIntList> input) -> bg::wp<NestedIntList> { return input; });
    const auto inner = bg::graph(
        "name"_m = "produce_inner_free_list",
        [&](bg::wp<bg::TS<std::int64_t>> first,
            bg::wp<bg::TS<std::int64_t>> second) -> bg::wp<NestedIntList> {
            return materialize(bg::tsl(first, second));
        });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return sum(bg::nested(inner, run, first, second)); };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array<std::int64_t, 4>{10, 20, 30, 40},
                                                std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a free list reference passes through a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference]") {
    const auto sum = bg::node<NestedListSum>();
    const auto inner = bg::graph(
        "name"_m = "pass_free_list",
        [](bg::wp<NestedIntList> input) -> bg::wp<NestedIntList> { return input; });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) {
        return sum(bg::nested(inner, run, bg::tsl(first, second)));
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array<std::int64_t, 4>{10, 20, 30, 40},
                                                std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

template <template <typename> typename Field> struct NestedIntBundle {
    Field<bg::TS<std::int64_t>> first;
    Field<bg::TS<std::int64_t>> second;
};

using NestedIntBundleTimeSeries = bg::TSB<NestedIntBundle>;

struct NestedBundleSum {
    bg::In<NestedIntBundleTimeSeries> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(*input->first + *input->second); }
};

TEST_CASE("a reference to an outer free bundle works in a nested graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedBundleSum>();
    const auto inner = bg::graph(
        "name"_m = "consume_outer_free_bundle",
        [&](bg::wp<NestedIntBundleTimeSeries> input) -> bg::wp<bg::TS<std::int64_t>> { return sum(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, bg::bundle("first"_m = first, "second"_m = second));
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array<std::int64_t, 4>{10, 20, 30, 40},
                                                std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a reference to an inner free bundle works in the outer graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedBundleSum>();
    const auto materialize = bg::graph(
        "name"_m = "materialize_free_bundle",
        [](bg::wp<NestedIntBundleTimeSeries> input) -> bg::wp<NestedIntBundleTimeSeries> { return input; });
    const auto inner = bg::graph(
        "name"_m = "produce_inner_free_bundle",
        [&](bg::wp<bg::TS<std::int64_t>> first,
            bg::wp<bg::TS<std::int64_t>> second) -> bg::wp<NestedIntBundleTimeSeries> {
            return materialize(bg::bundle("first"_m = first, "second"_m = second));
        });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return sum(bg::nested(inner, run, first, second)); };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array<std::int64_t, 4>{10, 20, 30, 40},
                                                std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a free bundle reference passes through a nested graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference]") {
    const auto sum = bg::node<NestedBundleSum>();
    const auto inner = bg::graph(
        "name"_m = "pass_free_bundle",
        [](bg::wp<NestedIntBundleTimeSeries> input) -> bg::wp<NestedIntBundleTimeSeries> { return input; });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) {
        return sum(bg::nested(inner, run, bg::bundle("first"_m = first, "second"_m = second)));
    };

    const auto actual = bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                                std::array<std::int64_t, 4>{10, 20, 30, 40},
                                                std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

} // namespace
