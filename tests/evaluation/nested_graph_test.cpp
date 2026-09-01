#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
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
    void evaluate() const { output.set(input.value() + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a nested graph can be stopped and started repeatedly", "[evaluation][nested][lifecycle]") {
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
    void evaluate() const { output.set(input.value() + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct ReferenceDouble {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(input.value() * 2); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a reference connects two nested graphs", "[evaluation][nested][reference]") {
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
    void evaluate() const { output.set(input.value() + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct NestedMiddlePassThrough {
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(input.value()); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a nested graph can own another nested graph", "[evaluation][nested][lifecycle][reference]") {
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

struct InnerTimer {
    static constexpr bool uses_future_scheduling = true;

    bg::In<bg::TS<std::int64_t>, bg::passive> input;
    bg::State<int> step;
    bg::Out<bg::TS<std::int64_t>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(input.value());
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        ++*step;
        output.set(input.value() + *step);
        if (*step != 2) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

TEST_CASE("future inner scheduling selects an outer cycle", "[evaluation][nested][scheduling]") {
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

struct NestedAliasedListSum {
    bg::In<bg::TSL<bg::TS<std::int64_t>, 2>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set(input[0].value() + input[1].value()); }
};

TEST_CASE("an aliased list processes reference changes in a nested graph",
          "[evaluation][nested][reference][tsl][alias]") {
    const auto sum = bg::node<NestedAliasedListSum>();
    const auto inner = bg::graph(
        "name"_m = "inner_aliased_list",
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

} // namespace
