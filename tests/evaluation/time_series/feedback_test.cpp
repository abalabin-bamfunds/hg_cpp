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

TEST_CASE("feedback accepts an explicit resolved output type",
          "[evaluation][feature:feedback][feature:type-system][series:value][scalar:i64]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback(bg::TS<bg::i64>::type());
        auto output = static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
        loop = input;
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 3>{2, 5, 7});
    CHECK(actual.equals({std::nullopt, 2, 5, 7}));
}

TEST_CASE("feedback captures a service-shaped dictionary of reference requests",
          "[evaluation][feature:feedback][feature:service][feature:reference][series:tsd]") {
    using namespace bg::literals;

    const auto service = bg::service<auto(bg::TS<bg::i64>)->bg::TS<bg::i64>>{"echo", {"value"}};
    const auto implementation = bg::graph(
        "name"_n = "echo_feedback",
        [](bg::wp<bg::TSB<bg::SCHEMA>, bg::var_kwargs> requests) {
            const auto request = bg::operators::getattr(requests, bg::util::symbol{"value"});
            auto loop = bg::feedback(bg::TSD<bg::service_client_id_t, bg::TS<bg::i64>>::type());
            const auto output = static_cast<bg::wp<bg::TSD<bg::service_client_id_t, bg::TS<bg::i64>>>>(loop);
            loop = request;
            return output;
        });
    auto catalogue = bg::ServiceCatalogue{};
    catalogue.register_impl(service, implementation);
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return service(input).as<bg::TS<bg::i64>>(); };

    try {
        const auto actual = bg::testing::test_graph(
            graph, bg::GraphRunOptions{.services = &catalogue}, std::array<bg::i64, 3>{2, 4, 6});
        CHECK(actual.equals({std::nullopt, 2, 4, 6}));
    } catch (const bg::TypeResolutionError &error) {
        auto diagnostic = std::string{"subject: "} + std::string{error.subject().text()};
        if (error.expected() != nullptr) {
            diagnostic += ", expected: ";
            error.expected()->format_name(diagnostic);
        }
        if (error.supplied() != nullptr) {
            diagnostic += ", supplied: ";
            error.supplied()->format_name(diagnostic);
        }
        INFO(diagnostic);
        throw;
    }
}

TEST_CASE("feedback can drive a bounded computation loop",
          "[evaluation][feature:feedback][feature:timed-scheduling][feature:test-graph]"
          "[series:value][scalar:i64][shape:chain][transition:update]") {
    const auto advance = bg::node([](bg::In<bg::TS<bg::i64>, bg::optional> seed,
                                     bg::In<bg::TS<bg::i64>, bg::optional> previous,
                                     bg::Out<bg::TS<bg::i64>> output) {
        if (previous.valid()) {
            if (*previous < 3) {
                output.set(*previous + 1);
            }
            return;
        }

        if (seed.valid()) {
            output.set(*seed);
        }
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> seed) {
        auto loop = bg::feedback<bg::TS<bg::i64>>("bounded");
        const auto next = advance(seed, loop);
        loop = next;
        return static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 1>{1});
    CHECK(actual.equals({std::nullopt, 1, 2, 3}));
}

TEST_CASE("feedback can drive a bounded computation loop in a nested graph",
          "[evaluation][feature:feedback][feature:timed-scheduling][feature:nested-graph]"
          "[feature:test-graph][series:value][scalar:i64][shape:nested][transition:update]") {
    using namespace bg::literals;

    const auto advance = bg::node([](bg::In<bg::TS<bg::i64>, bg::optional> seed,
                                     bg::In<bg::TS<bg::i64>, bg::optional> previous,
                                     bg::Out<bg::TS<bg::i64>> output) {
        if (previous.valid()) {
            if (*previous < 3) {
                output.set(*previous + 1);
            }
            return;
        }

        if (seed.valid()) {
            output.set(*seed);
        }
    });
    const auto inner = bg::graph("name"_n = "bounded_feedback", [&](bg::wp<bg::TS<bg::i64>> seed) {
        auto loop = bg::feedback<bg::TS<bg::i64>>("nested");
        const auto next = advance(seed, loop);
        loop = next;
        return static_cast<bg::wp<bg::TS<bg::i64>>>(loop);
    });
    const auto outer = [&](bg::wp<bg::TS<bg::i64>> seed, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, seed);
    };

    const auto actual = bg::testing::test_graph(outer, std::array<bg::i64, 1>{1}, std::array<bool, 1>{true});
    CHECK(actual.equals({std::nullopt, 1, 2, 3}));
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

    wiring.finalize();
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
    const auto sum = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                bg::Out<bg::TS<bg::i64>> output) {
        auto total = bg::i64{};
        input.for_each_value([&](const auto &item) { total += *item; });
        output.set(total);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        auto loop = bg::feedback<bg::TSD<bg::i64, bg::TS<bg::i64>>>();
        auto output = sum(static_cast<bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>>>(loop));
        loop = input;
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, {{{1, 1}}, {{1, 2}}, {{1, literal::REMOVE}}});
    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("feedback samples switched dictionary references before retaining their deltas",
          "[evaluation][feature:feedback][feature:reference][feature:sampling][series:tsd]"
          "[scalar:i64][binding:reference-to-value][transition:addition][transition:removal]") {
    const auto select = bg::node([](bg::In<bg::TS<bool>> use_second,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto changes = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto added = bg::i64{};
        auto removed = bg::i64{};
        input.for_each_added([&](const auto &key) { added += *key; });
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(added * 100 + removed);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second) {
        auto loop = bg::feedback<bg::TSD<bg::i64, bg::TS<bg::i64>>>();
        const auto output = changes(loop);
        loop = select(use_second, first, second);
        return output;
    };

    const auto actual = bg::testing::test_graph(
        graph, {false, true}, {{{1, 10}}, {{1, 10}}}, {{{2, 20}}, {{2, 20}}});
    CHECK(actual.equals({std::nullopt, 100, 201}));
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

TEST_CASE("feedback rejects reference time series",
          "[feature:feedback][feature:wiring][series:reference][scalar:i64]") {
    auto wiring = bg::GraphWiring{};
    auto context = bg::WiringContext{wiring};

    CHECK_THROWS_AS((bg::feedback<bg::REF<bg::TS<bg::i64>>>()), bg::WiringError);
}

TEST_CASE("feedback rejects collections containing references",
          "[feature:feedback][feature:wiring][series:tsd][scalar:i64]") {
    auto wiring = bg::GraphWiring{};
    auto context = bg::WiringContext{wiring};

    CHECK_THROWS_AS((bg::feedback<bg::TSD<bg::i64, bg::REF<bg::TS<bg::i64>>>>()), bg::WiringError);
}
