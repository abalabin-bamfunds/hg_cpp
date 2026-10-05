#include <bg/feedback.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <stdexcept>

TEST_CASE("feedback replays each captured value in the next cycle",
          "[evaluation][feature:feedback][feature:timed-scheduling][series:value][scalar:i64]"
          "[transition:update]") {
    const auto observe = bg::node([](bg::In<bg::TS<bg::i64>, bg::optional>) {});
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<bg::TS<bg::i64>>("loop");
        observe(loop);
        auto output = static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
        loop = input;
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 4>{2, 5, 7, 11});
    CHECK(actual.equals({std::nullopt, 2, 5, 7, 11}));
}

TEST_CASE("feedback waits for its first input tick",
          "[evaluation][feature:feedback][series:value][scalar:i64][transition:first-tick]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<bg::TS<bg::i64>>();
        auto output = static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
        loop = input;
        return output;
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<std::optional<bg::i64>, 4>{std::nullopt, 3, std::nullopt, 8});
    CHECK(actual.equals({std::nullopt, std::nullopt, 3, std::nullopt, 8}));
}

TEST_CASE("feedback wires through the normal guarantee compatibility rules",
          "[feature:feedback][feature:type-system][series:value][scalar:i64][guarantee:final]") {
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        auto loop = bg::feedback<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>();
        loop = bg::operators::const_(7).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        static_cast<void>(static_cast<bg::wp<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>>(loop));
    }

    CHECK_NOTHROW(bg::CompiledGraph{wiring});
}

TEST_CASE("a feedback sink can be assigned only once",
          "[feature:feedback][series:value][scalar:i64]") {
    auto wiring = bg::GraphWiring{};
    auto context = bg::WiringContext{wiring};
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto loop = bg::feedback<bg::TS<bg::i64>>();
    loop = source();

    CHECK_THROWS_AS(loop = source(), std::logic_error);
}

TEST_CASE("feedback replays invalidation and revalidation",
          "[evaluation][feature:feedback][feature:validity][series:value][scalar:i64]"
          "[transition:invalidation][transition:revalidation]") {
    const auto invalidate_on_zero = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                                bg::Out<bg::TS<bg::i64>> output) {
        if (*input == 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<bg::TS<bg::i64>>();
        auto output = static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
        loop = invalidate_on_zero(input);
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 4>{1, 2, 0, 3});
    CHECK(actual.equals({std::nullopt, 1, 2, std::nullopt, 3}));
}

TEST_CASE("separate feedback handles retain separate ticks",
          "[evaluation][feature:feedback][series:value][scalar:i64][shape:multiple]") {
    const auto scale = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input * 10);
    });
    const auto add = bg::node([](bg::In<bg::TS<bg::i64>> first, bg::In<bg::TS<bg::i64>> second,
                                 bg::Out<bg::TS<bg::i64>> output) { output.set(*first + *second); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto first = bg::feedback<bg::TS<bg::i64>>("first");
        auto second = bg::feedback<bg::TS<bg::i64>>("second");
        auto output = add(static_cast<bg::wp<bg::TS<bg::i64>>>(first),
                          static_cast<bg::wp<bg::TS<bg::i64>>>(second));
        first = input;
        second = scale(input);
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 2>{2, 3});
    CHECK(actual.equals({std::nullopt, 22, 33}));
}

TEST_CASE("feedback replays TSD item changes",
          "[evaluation][feature:feedback][series:tsd][scalar:i64][transition:addition]"
          "[transition:update][transition:removal]") {
    const auto update = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                     bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>> output) {
        if (*input == 0) {
            output.erase(1);
            return;
        }
        output[1].set(*input);
    });
    const auto sum = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                bg::Out<bg::TS<bg::i64>> output) {
        auto total = bg::i64{};
        input.for_each_value([&](const auto &item) { total += *item; });
        output.set(total);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<bg::TSD<bg::i64, bg::TS<bg::i64>>>();
        auto output = sum(static_cast<bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>>>(loop));
        loop = update(input);
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 3>{1, 2, 0});
    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("feedback source and sink must be in the same graph",
          "[feature:feedback][feature:wiring][series:value][scalar:i64]") {
    auto first = bg::GraphWiring{};
    auto second = bg::GraphWiring{};
    auto first_context = bg::WiringContext{first};
    auto loop = bg::feedback<bg::TS<bg::i64>>();
    {
        auto second_context = bg::WiringContext{second};
        const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
        CHECK_THROWS_AS(loop = source(), bg::WiringError);
    }
}
