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

struct FinalizingInner {
    bg::In<bg::TS<std::int64_t>> input;
    bg::State<int> marker;
    bg::Out<bg::TS<std::int64_t>> output;

    void start(bg::EvaluationContext *context) const noexcept {
        *marker = 1;
        if (global_state == nullptr) {
            global_state = &context->global_state();
        }
    }

    void evaluate() const { output.set(*input); }

    void finalize(bg::EvaluationContext *context) const noexcept {
        if (finalization_count < finalization_times.size()) {
            finalization_times[finalization_count] = context->evaluation_time();
            correct_state[finalization_count] = *marker == 1 && &context->global_state() == global_state;
        }
        ++finalization_count;
    }

    inline static bg::GlobalState *global_state{};
    inline static std::array<bg::EvaluationTime, 2> finalization_times{};
    inline static std::array<bool, 2> correct_state{};
    inline static std::size_t finalization_count{};
};

struct NestedTransitionSink {
    bg::In<bg::TS<std::int64_t>, bg::optional> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate(bg::EvaluationContext *context) const {
        observed.push_back(
            {input.valid(), input.became_valid(), input.became_invalid(), input.modified(),
             input.last_updated() == context->evaluation_time()});
        if (input.valid()) {
            output.set(*input);
        }
    }

    inline static std::vector<std::array<bool, 5>> observed;
};

TEST_CASE("a nested graph can be stopped and started repeatedly",
          "[evaluation][nested][lifecycle][feature:nested-graph][shape:nested][transition:start]"
          "[transition:stop][transition:restart][scalar:boolean][scalar:integer][series:value]") {
    const auto increment = bg::node<LifecycleIncrement>();
    const auto inner =
        bg::graph("name"_n = "inner_increment",
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

TEST_CASE("inner graph finalization uses the outer cycle's global state",
          "[evaluation][nested][lifecycle][feature:finalization][transition:removal]") {
    const auto finalizing = bg::node<FinalizingInner>();
    const auto inner = bg::graph("name"_n = "finalizing_inner", [&](bg::wp<bg::TS<std::int64_t>> input) {
        return finalizing(input);
    });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, input);
    };
    FinalizingInner::global_state = nullptr;
    FinalizingInner::finalization_count = 0;

    const auto actual = bg::testing::test_graph(outer, std::array{1, 2, 3, 4},
                                                 std::array{true, false, false, true});

    CHECK(actual.equals({1, std::nullopt, std::nullopt, 4}));
    CHECK(FinalizingInner::finalization_count == 2);
    CHECK(FinalizingInner::finalization_times == std::array{bg::MIN_DT + std::chrono::microseconds{3},
                                                           bg::MIN_DT + std::chrono::microseconds{4}});
    CHECK(FinalizingInner::correct_state == std::array{true, true});
}

TEST_CASE("a nested boundary samples an existing valid input when it starts",
          "[evaluation][nested][reference][feature:validity][transition:first-tick][validity:optional]"
          "[series:value][binding:value-to-reference]") {
    const auto sink = bg::node<NestedTransitionSink>();
    const auto inner = bg::graph("name"_n = "nested_transition_sink",
                                 [&](bg::wp<bg::TS<std::int64_t>> input) { return sink(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, input);
    };
    NestedTransitionSink::observed.clear();

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 3>{10, std::nullopt, std::nullopt},
        std::array{false, false, true});

    CHECK(actual.equals({std::nullopt, std::nullopt, 10}));
    CHECK(NestedTransitionSink::observed ==
          std::vector<std::array<bool, 5>>{{true, true, false, true, true}});
}

TEST_CASE("a dynamic input controls scheduling inside a nested graph",
          "[feature:input-activity][feature:nested-graph][series:value][definition:function]"
          "[shape:nested][activity:active][transition:update]") {
    const auto gate =
        bg::node([](bg::In<bg::TS<std::int64_t>, bg::dynamic_activity> input, bg::Out<bg::TS<std::int64_t>> output) {
            output.set(*input);
            input.make_passive();
        });
    const auto inner =
        bg::graph("name"_n = "dynamic_input", [&](bg::wp<bg::TS<std::int64_t>> input) { return gate(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, input);
    };

    CHECK(bg::testing::test_graph(outer, std::array<std::int64_t, 2>{1, 2}, std::array{true, true})
              .equals({1, std::nullopt}));
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
        bg::graph("name"_n = "first",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return increment(input); });
    const auto second =
        bg::graph("name"_n = "second",
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
        bg::graph("name"_n = "leaf",
                  [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> { return increment(input); });
    const auto middle =
        bg::graph("name"_n = "middle",
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

struct InnerTimer {
    static constexpr bool uses_timed_scheduling = true;

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

TEST_CASE("timed inner scheduling selects an outer cycle",
          "[evaluation][nested][scheduling][feature:nested-graph][shape:nested][scalar:integer]"
          "[series:value]") {
    const auto timer = bg::node<InnerTimer>();
    const auto inner =
        bg::graph("name"_n = "inner_timer",
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
        "name"_n = "inner_free_list",
        [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second) -> bg::wp<bg::TS<std::int64_t>> {
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

struct NestedListSum {
    bg::In<bg::TSL<bg::TS<std::int64_t>, 2>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

TEST_CASE("a reference to an outer free list works in a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedListSum>();
    const auto inner = bg::graph(
        "name"_n = "consume_outer_free_list",
        [&](bg::wp<bg::TSL<bg::TS<std::int64_t>, 2>> input) -> bg::wp<bg::TS<std::int64_t>> { return sum(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return bg::nested(inner, run, bg::tsl(first, second)); };

    const auto actual =
        bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                std::array<std::int64_t, 4>{10, 20, 30, 40}, std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a reference to an inner free list works in the outer graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedListSum>();
    const auto materialize =
        bg::graph("name"_n = "materialize_free_list",
                  [](bg::wp<bg::TSL<bg::TS<std::int64_t>, 2>> input) -> bg::wp<bg::TSL<bg::TS<std::int64_t>, 2>> {
                      return input;
                  });
    const auto inner = bg::graph("name"_n = "produce_inner_free_list",
                                 [&](bg::wp<bg::TS<std::int64_t>> first,
                                     bg::wp<bg::TS<std::int64_t>> second) -> bg::wp<bg::TSL<bg::TS<std::int64_t>, 2>> {
                                     return materialize(bg::tsl(first, second));
                                 });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return sum(bg::nested(inner, run, first, second)); };

    const auto actual =
        bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                std::array<std::int64_t, 4>{10, 20, 30, 40}, std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a free list reference passes through a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference]") {
    const auto sum = bg::node<NestedListSum>();
    const auto inner =
        bg::graph("name"_n = "pass_free_list",
                  [](bg::wp<bg::TSL<bg::TS<std::int64_t>, 2>> input) -> bg::wp<bg::TSL<bg::TS<std::int64_t>, 2>> {
                      return input;
                  });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return sum(bg::nested(inner, run, bg::tsl(first, second))); };

    const auto actual =
        bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                std::array<std::int64_t, 4>{10, 20, 30, 40}, std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("an outer free list reference item rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select =
        bg::node([](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<std::int64_t>>> first,
                    bg::In<bg::REF<bg::TS<std::int64_t>>> second,
                    bg::Out<bg::REF<bg::TS<std::int64_t>>> output) { output.copy(*use_second ? second : first); });
    const auto add = bg::node([](bg::In<bg::TS<std::int64_t>> left, bg::In<bg::TS<std::int64_t>> right,
                                 bg::Out<bg::TS<std::int64_t>> output) { output.set(*left + *right); });
    const auto inner =
        bg::graph("name"_n = "consume_outer_free_reference_list",
                  [&](bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>> input)
                      -> bg::wp<bg::TS<std::int64_t>> { return add(input[0], input[1]); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value,
                           bg::wp<bg::TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        return bg::nested(inner, run, bg::tsl(value, selected));
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("an inner free list reference item rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select =
        bg::node([](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<std::int64_t>>> first,
                    bg::In<bg::REF<bg::TS<std::int64_t>>> second,
                    bg::Out<bg::REF<bg::TS<std::int64_t>>> output) { output.copy(*use_second ? second : first); });
    const auto add = bg::node([](bg::In<bg::TS<std::int64_t>> left, bg::In<bg::TS<std::int64_t>> right,
                                 bg::Out<bg::TS<std::int64_t>> output) { output.set(*left + *right); });
    const auto materialize = bg::graph(
        "name"_n = "materialize_free_reference_list",
        [](bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>> input)
            -> bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>> { return input; });
    const auto inner =
        bg::graph("name"_n = "produce_inner_free_reference_list",
                  [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                      bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value)
                      -> bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>> {
                      const auto selected = select(use_second, first, second);
                      return materialize(bg::tsl(value, selected));
                  });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value,
                           bg::wp<bg::TS<bool>> run) {
        const auto reference = bg::nested(inner, run, first, second, use_second, value);
        const auto list =
            bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>>{reference};
        return add(list[0], list[1]);
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("a free list reference item survives two boundaries and repeated lifecycle changes",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:doubly-nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]"
          "[transition:stop][transition:restart]") {
    const auto select =
        bg::node([](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<std::int64_t>>> first,
                    bg::In<bg::REF<bg::TS<std::int64_t>>> second,
                    bg::Out<bg::REF<bg::TS<std::int64_t>>> output) { output.copy(*use_second ? second : first); });
    const auto add = bg::node([](bg::In<bg::TS<std::int64_t>> left, bg::In<bg::TS<std::int64_t>> right,
                                 bg::Out<bg::TS<std::int64_t>> output) { output.set(*left + *right); });
    const auto pass_through = bg::graph(
        "name"_n = "pass_free_reference_list",
        [](bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>> input)
            -> bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>> { return input; });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value,
                           bg::wp<bg::TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        const auto list = bg::tsl(value, selected);
        const auto first_boundary = bg::nested(pass_through, run, list);
        const auto second_boundary = bg::nested(pass_through, run, first_boundary);
        const auto result =
            bg::wp<bg::TSL<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>, 2>>{second_boundary};
        return add(result[0], result[1]);
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 8>{10, 20, 30, std::nullopt, 40, std::nullopt, std::nullopt, 50},
        std::array<std::optional<std::int64_t>, 8>{100, 200, std::nullopt, 300, 400, std::nullopt, 500, std::nullopt},
        std::array<std::optional<bool>, 8>{false, true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt,
                                           std::nullopt},
        std::array<std::optional<std::int64_t>, 8>{1, 2, std::nullopt, std::nullopt, 4, std::nullopt, std::nullopt,
                                                   std::nullopt},
        std::array<std::optional<bool>, 8>{true, std::nullopt, std::nullopt, false, std::nullopt, true, std::nullopt,
                                           std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, std::nullopt, std::nullopt, 44, std::nullopt, 54}));
}

template <template <typename, auto...> typename Field> struct NestedIntBundle {
    Field<bg::TS<std::int64_t>> first;
    Field<bg::TS<std::int64_t>> second;
};

struct NestedBundleSum {
    bg::In<bg::TSB<NestedIntBundle>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(*input->first + *input->second); }
};

TEST_CASE("a reference to an outer free bundle works in a nested graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedBundleSum>();
    const auto inner =
        bg::graph("name"_n = "consume_outer_free_bundle",
                  [&](bg::wp<bg::TSB<NestedIntBundle>> input) -> bg::wp<bg::TS<std::int64_t>> { return sum(input); });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, bg::bundle("first"_n = first, "second"_n = second));
    };

    const auto actual =
        bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                std::array<std::int64_t, 4>{10, 20, 30, 40}, std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a reference to an inner free bundle works in the outer graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = bg::node<NestedBundleSum>();
    const auto materialize =
        bg::graph("name"_n = "materialize_free_bundle",
                  [](bg::wp<bg::TSB<NestedIntBundle>> input) -> bg::wp<bg::TSB<NestedIntBundle>> { return input; });
    const auto inner = bg::graph("name"_n = "produce_inner_free_bundle",
                                 [&](bg::wp<bg::TS<std::int64_t>> first,
                                     bg::wp<bg::TS<std::int64_t>> second) -> bg::wp<bg::TSB<NestedIntBundle>> {
                                     return materialize(bg::bundle("first"_n = first, "second"_n = second));
                                 });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) { return sum(bg::nested(inner, run, first, second)); };

    const auto actual =
        bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                std::array<std::int64_t, 4>{10, 20, 30, 40}, std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a free bundle reference passes through a nested graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference]") {
    const auto sum = bg::node<NestedBundleSum>();
    const auto inner =
        bg::graph("name"_n = "pass_free_bundle",
                  [](bg::wp<bg::TSB<NestedIntBundle>> input) -> bg::wp<bg::TSB<NestedIntBundle>> { return input; });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> run) {
        return sum(bg::nested(inner, run, bg::bundle("first"_n = first, "second"_n = second)));
    };

    const auto actual =
        bg::testing::test_graph(outer, std::array<std::int64_t, 4>{1, 2, 3, 4},
                                std::array<std::int64_t, 4>{10, 20, 30, 40}, std::array{true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

template <template <typename, auto...> typename Field> struct NestedReferenceBundle {
    Field<bg::TS<std::int64_t>> value;
    Field<bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>> selected;
};

TEST_CASE("an outer free bundle reference field rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select =
        bg::node([](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<std::int64_t>>> first,
                    bg::In<bg::REF<bg::TS<std::int64_t>>> second,
                    bg::Out<bg::REF<bg::TS<std::int64_t>>> output) { output.copy(*use_second ? second : first); });
    const auto add = bg::node([](bg::In<bg::TS<std::int64_t>> left, bg::In<bg::TS<std::int64_t>> right,
                                 bg::Out<bg::TS<std::int64_t>> output) { output.set(*left + *right); });
    const auto inner = bg::graph("name"_n = "consume_outer_free_reference_bundle",
                                 [&](bg::wp<bg::TSB<NestedReferenceBundle>> input) -> bg::wp<bg::TS<std::int64_t>> {
                                     return add(input->value, input->selected);
                                 });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value,
                           bg::wp<bg::TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        const auto bundle = bg::bundle("value"_n = value, "selected"_n = selected);
        return bg::nested(inner, run, bundle);
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("an inner free bundle reference field rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select =
        bg::node([](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<std::int64_t>>> first,
                    bg::In<bg::REF<bg::TS<std::int64_t>>> second,
                    bg::Out<bg::REF<bg::TS<std::int64_t>>> output) { output.copy(*use_second ? second : first); });
    const auto add = bg::node([](bg::In<bg::TS<std::int64_t>> left, bg::In<bg::TS<std::int64_t>> right,
                                 bg::Out<bg::TS<std::int64_t>> output) { output.set(*left + *right); });
    const auto materialize = bg::graph(
        "name"_n = "materialize_free_reference_bundle",
        [](bg::wp<bg::TSB<NestedReferenceBundle>> input) -> bg::wp<bg::TSB<NestedReferenceBundle>> { return input; });
    const auto inner = bg::graph("name"_n = "produce_inner_free_reference_bundle",
                                 [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                                     bg::wp<bg::TS<bool>> use_second,
                                     bg::wp<bg::TS<std::int64_t>> value) -> bg::wp<bg::TSB<NestedReferenceBundle>> {
                                     const auto selected = select(use_second, first, second);
                                     return materialize(bg::bundle("value"_n = value, "selected"_n = selected));
                                 });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value,
                           bg::wp<bg::TS<bool>> run) {
        const auto reference = bg::nested(inner, run, first, second, use_second, value);
        const auto bundle = bg::wp<bg::TSB<NestedReferenceBundle>>{reference};
        return add(bundle->value, bundle->selected);
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("a free bundle reference field survives two boundaries and repeated lifecycle changes",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:doubly-nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]"
          "[transition:stop][transition:restart]") {
    const auto select =
        bg::node([](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<std::int64_t>>> first,
                    bg::In<bg::REF<bg::TS<std::int64_t>>> second,
                    bg::Out<bg::REF<bg::TS<std::int64_t>>> output) { output.copy(*use_second ? second : first); });
    const auto add = bg::node([](bg::In<bg::TS<std::int64_t>> left, bg::In<bg::TS<std::int64_t>> right,
                                 bg::Out<bg::TS<std::int64_t>> output) { output.set(*left + *right); });
    const auto pass_through = bg::graph(
        "name"_n = "pass_free_reference_bundle",
        [](bg::wp<bg::TSB<NestedReferenceBundle>> input) -> bg::wp<bg::TSB<NestedReferenceBundle>> { return input; });
    const auto outer = [&](bg::wp<bg::TS<std::int64_t>> first, bg::wp<bg::TS<std::int64_t>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<std::int64_t>> value,
                           bg::wp<bg::TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        const auto bundle = bg::bundle("value"_n = value, "selected"_n = selected);
        const auto first_boundary = bg::nested(pass_through, run, bundle);
        const auto second_boundary = bg::nested(pass_through, run, first_boundary);
        const auto result = bg::wp<bg::TSB<NestedReferenceBundle>>{second_boundary};
        return add(result->value, result->selected);
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array<std::optional<std::int64_t>, 8>{10, 20, 30, std::nullopt, 40, std::nullopt, std::nullopt, 50},
        std::array<std::optional<std::int64_t>, 8>{100, 200, std::nullopt, 300, 400, std::nullopt, 500, std::nullopt},
        std::array<std::optional<bool>, 8>{false, true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt,
                                           std::nullopt},
        std::array<std::optional<std::int64_t>, 8>{1, 2, std::nullopt, std::nullopt, 4, std::nullopt, std::nullopt,
                                                   std::nullopt},
        std::array<std::optional<bool>, 8>{true, std::nullopt, std::nullopt, false, std::nullopt, true, std::nullopt,
                                           std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, std::nullopt, std::nullopt, 44, std::nullopt, 54}));
}

} // namespace
