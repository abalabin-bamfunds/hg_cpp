#include <bg/feedback.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/operators.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>

using namespace bg;

TEST_CASE("feedback replays each captured value in the next cycle",
          "[evaluation][feature:feedback][feature:timed-scheduling][series:value][scalar:i64]"
          "[transition:update]") {
    const auto graph = [](wp<TS<i64>> input) {
        auto loop = feedback<TS<i64>>("loop");
        loop = input;
        return static_cast<wp<TS<i64>>>(loop);
    };

    const auto actual = testing::test_graph(graph, {2, 5, 7, 11});
    CHECK(actual.equals({std::nullopt, 2, 5, 7, 11}));
}

TEST_CASE("feedback waits for its first input tick",
          "[evaluation][feature:feedback][series:value][scalar:i64][transition:first-tick]") {
    const auto graph = [](wp<TS<i64>> input) {
        auto loop = feedback<TS<i64>>();
        loop = input;
        return static_cast<wp<TS<i64>>>(loop);
    };

    const auto actual = testing::test_graph(graph, {std::nullopt, 3, std::nullopt, 8});
    CHECK(actual.equals({std::nullopt, std::nullopt, 3, std::nullopt, 8}));
}

TEST_CASE("feedback can drive a bounded computation loop",
          "[evaluation][feature:feedback][feature:timed-scheduling][feature:test-graph]"
          "[series:value][scalar:i64][shape:chain][transition:update]") {
    const auto advance = node([](std::optional<i64> seed, std::optional<i64> previous) -> std::optional<i64> {
        if (previous) {
            return *previous < 3 ? std::optional<i64>{*previous + 1} : std::nullopt;
        }
        return seed;
    });

    const auto graph = [&](wp<TS<i64>> seed) {
        auto loop = feedback<TS<i64>>("bounded");
        const auto next = advance(seed, loop);
        loop = next;
        return static_cast<wp<TS<i64>>>(loop);
    };

    const auto actual = testing::test_graph(graph, {1});
    CHECK(actual.equals({std::nullopt, 1, 2, 3}));
}

TEST_CASE("separate feedback handles retain separate ticks",
          "[evaluation][feature:feedback][series:value][scalar:i64][shape:multiple]") {
    const auto scale = node([](i64 input) { return input * 10; });

    const auto add = node([](i64 first, i64 second) { return first + second; });

    const auto graph = [&](wp<TS<i64>> input) {
        auto first = feedback<TS<i64>>("first");
        auto second = feedback<TS<i64>>("second");
        const auto output = add(first, second);
        first = input;
        second = scale(input);
        return output;
    };

    const auto actual = testing::test_graph(graph, {2, 3});
    CHECK(actual.equals({std::nullopt, 22, 33}));
}

TEST_CASE("feedback replays invalidation and revalidation",
          "[evaluation][feature:feedback][feature:validity][series:value][scalar:i64]"
          "[transition:invalidation][transition:revalidation]") {
    using namespace literals;

    const auto invalidate_on_zero = node([](i64 input, Out<TS<i64>> output) {
        if (input == 0) {
            output.invalidate();
            return;
        }
        output.set(input);
    });

    const auto replay = graph("name"_n = "replay", [&](wp<TS<i64>> input) {
        auto loop = feedback<TS<i64>>();
        loop = invalidate_on_zero(input);
        return static_cast<wp<TS<i64>>>(loop);
    });

    CHECK(testing::test_graph(replay, {1, 2, 0, 3}).equals({literal::SKIP, 1, 2, literal::INVALID, 3}));
}

TEST_CASE("feedback replays TSD item changes",
          "[evaluation][feature:feedback][series:tsd][scalar:i64][transition:addition]"
          "[transition:update][transition:removal]") {
    const auto sum = node([](In<TSD<i64, TS<i64>>> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &item) { total += *item; });
        return total;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) {
        auto loop = feedback<TSD<i64, TS<i64>>>();
        auto output = sum(loop);
        loop = input;
        return output;
    };

    const auto actual = testing::test_graph(graph, {{{1, 1}}, {{1, 2}}, {{1, literal::REMOVE}}});
    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("feedback accepts an explicit resolved output type",
          "[evaluation][feature:feedback][feature:type-system][series:value][scalar:i64]") {
    const auto graph = [](wp<TS<i64>> input) {
        auto loop = feedback(TS<i64>::type());
        loop = input;
        return static_cast<wp<TS<i64>>>(loop);
    };

    const auto actual = testing::test_graph(graph, {2, 5, 7});
    CHECK(actual.equals({std::nullopt, 2, 5, 7}));
}

TEST_CASE("feedback infers a reference-free output type from a wiring point",
          "[evaluation][feature:feedback][feature:type-system][series:value][scalar:i64]") {
    const auto graph = [](wp<TS<i64>> input) {
        return wp<TS<i64>>{feedback(static_cast<WiringPoint>(input))};
    };

    const auto actual = testing::test_graph(graph, {2, 5, 7});
    CHECK(actual.equals({std::nullopt, 2, 5, 7}));
}

TEST_CASE("feedback inference changes a const source to a final output",
          "[feature:feedback][feature:type-system][series:value][guarantee:final]") {
    const auto graph = [] {
        const auto input = operators::const_(i64{7});
        const auto output = feedback(static_cast<WiringPoint>(input));
        CHECK(&output.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        return wp<TS<i64, TimeSeriesGuarantee::final>>{output};
    };

    const auto actual = testing::test_graph(graph);
    CHECK(actual.equals({std::nullopt, 7}));
}

TEST_CASE("feedback can drive a bounded computation loop in a nested graph",
          "[evaluation][feature:feedback][feature:timed-scheduling][feature:nested-graph]"
          "[feature:test-graph][series:value][scalar:i64][shape:nested][transition:update]") {
    using namespace literals;

    const auto advance = node([](std::optional<i64> seed, std::optional<i64> previous) -> std::optional<i64> {
        if (previous) {
            return *previous < 3 ? std::optional<i64>{*previous + 1} : std::nullopt;
        }
        return seed;
    });

    const auto inner = graph("name"_n = "bounded_feedback", [&](wp<TS<i64>> seed) {
        auto loop = feedback<TS<i64>>("nested");
        const auto next = advance(seed, loop);
        loop = next;
        return static_cast<wp<TS<i64>>>(loop);
    });

    const auto outer = [&](wp<TS<i64>> seed, wp<TS<bool>> run) {
        return nested(inner, run, seed);
    };

    const auto actual = testing::test_graph(outer, {1}, {true});
    CHECK(actual.equals({std::nullopt, 1, 2, 3}));
}

TEST_CASE("feedback captures a service-shaped dictionary of reference requests",
          "[evaluation][feature:feedback][feature:service][feature:reference][series:tsd]") {
    using namespace literals;

    const auto service = bg::service<auto(TS<i64>) -> TS<i64>>{"echo", {"value"}};

    const auto implementation = graph(
        "name"_n = "echo_feedback",
        [](wp<TSB<SCHEMA>, var_kwargs> requests) {
            const auto request = operators::getattr(requests, util::symbol{"value"});
            auto loop = feedback(TSD<service_client_id_t, TS<i64>>::type());
            const auto output = static_cast<wp<TSD<service_client_id_t, TS<i64>>>>(loop);
            loop = request;
            return output;
        });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service, implementation);

    const auto echo = [&](wp<TS<i64>> input) { return service(input).as<TS<i64>>(); };

    const auto actual = testing::test_graph(echo, GraphRunOptions{.services = &catalogue}, {2, 4, 6});
    CHECK(actual.equals({std::nullopt, 2, 4, 6}));
}

TEST_CASE("feedback samples switched dictionary references before retaining their deltas",
          "[evaluation][feature:feedback][feature:reference][feature:sampling][series:tsd]"
          "[scalar:i64][binding:reference-to-value][transition:addition][transition:removal]") {
    const auto select = node([](In<TS<bool>> use_second,
                                In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });

    const auto graph = [&](wp<TS<bool>> use_second,
                           wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        auto loop = feedback<TSD<i64, TS<i64>>>();
        const auto output = static_cast<wp<TSD<i64, TS<i64>>>>(loop);
        loop = select(use_second, first, second);
        return output;
    };

    const auto actual = testing::test_graph(graph, {false, true}, {{{1, 10}}, {{1, 10}}}, {{{2, 20}}, {{2, 20}}});
    CHECK(actual.equals({literal::SKIP, {{1, 10}}, {{1, literal::REMOVE}, {2, 20}}}));
}

TEST_CASE("feedback wires through the normal guarantee compatibility rules",
          "[feature:feedback][feature:type-system][series:value][scalar:i64][guarantee:final]") {
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        auto loop = feedback<TS<i64, TimeSeriesGuarantee::final>>();
        loop = operators::const_(i64{7});
        static_cast<void>(static_cast<wp<TS<i64, TimeSeriesGuarantee::final>>>(loop));
    }

    wiring.finalize();
    CHECK_NOTHROW(CompiledGraph{wiring});
}

TEST_CASE("a feedback sink can be assigned only once",
          "[feature:feedback][series:value][scalar:i64]") {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};

    const auto source = node([](Out<TS<i64>>) {});
    auto loop = feedback<TS<i64>>();
    loop = source();

    CHECK_THROWS_AS(loop = source(), std::logic_error);
}

TEST_CASE("feedback source and sink must be in the same graph",
          "[feature:feedback][feature:wiring][series:value][scalar:i64]") {
    auto first = GraphWiring{};
    auto second = GraphWiring{};
    auto first_context = WiringContext{first};
    auto loop = feedback<TS<i64>>();
    {
        auto second_context = WiringContext{second};

        const auto source = node([](Out<TS<i64>>) {});
        CHECK_THROWS_AS(loop = source(), WiringError);
    }
}

TEST_CASE("feedback rejects reference time series",
          "[feature:feedback][feature:wiring][series:reference][scalar:i64]") {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};

    CHECK_THROWS_AS((feedback<REF<TS<i64>>>()), WiringError);
}

TEST_CASE("feedback rejects collections containing references",
          "[feature:feedback][feature:wiring][series:tsd][scalar:i64]") {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};

    CHECK_THROWS_AS((feedback<TSD<i64, REF<TS<i64>>>>()), WiringError);
}
