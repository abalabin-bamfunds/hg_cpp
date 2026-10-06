#include <bg/feedback.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <stdexcept>

TEST_CASE("feedback replays each captured value in the next cycle",
          "[evaluation][feature:scheduling][series:value][scalar:integer][transition:update]") {
    const auto observe = bg::node([](bg::In<bg::TS<bg::i64>, bg::optional>) {});
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<bg::TS<bg::i64>>("loop");
        observe(loop);
        auto output = static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
        loop = input;
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 4>{2, 5, 7, 11});
    CHECK(actual.equals({std::nullopt, 2, 5, 7}));
}

TEST_CASE("feedback waits for its first input tick",
          "[evaluation][feature:scheduling][series:value][scalar:integer][transition:first-tick]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<bg::TS<bg::i64>>();
        auto output = static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
        loop = input;
        return output;
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<std::optional<bg::i64>, 4>{std::nullopt, 3, std::nullopt, 8});
    CHECK(actual.equals({std::nullopt, std::nullopt, 3, std::nullopt}));
}

TEST_CASE("feedback wires through the normal guarantee compatibility rules",
          "[evaluation][feature:type-system][series:value][scalar:integer][guarantee:final][guarantee:const]") {
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
          "[evaluation][feature:scheduling][series:value][scalar:integer]") {
    auto wiring = bg::GraphWiring{};
    auto context = bg::WiringContext{wiring};
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto loop = bg::feedback<bg::TS<bg::i64>>();
    loop = source();

    CHECK_THROWS_AS(loop = source(), std::logic_error);
}
