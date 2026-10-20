#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

struct fallback_definition {
    bg::State<int> invalidation_count;
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(*input);
    }

    void on_invalidation() const {
        ++*invalidation_count;
        output.set(-static_cast<float>(*invalidation_count));
    }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("required-input invalidation propagates through a graph",
          "[evaluation][invalidation][feature:invalidation][shape:chain][validity:required]"
          "[transition:invalidation][transition:revalidation][series:value][scalar:floating-point]") {
    auto copy_evaluations = 0;
    auto consumed_values = std::vector<float>{};
    const auto copy = bg::node([&](bg::In<bg::TS<float>> input, bg::Out<bg::TS<float>> output) {
        ++copy_evaluations;
        output.set(*input);
    });
    const auto consume = bg::node([&](bg::In<bg::TS<float>> input) { consumed_values.push_back(*input); });
    const auto observe_validity = bg::node([](bg::In<bg::TS<float>, bg::optional> input,
                                               bg::Out<bg::TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](bg::wp<bg::TS<float>> input) {
        const auto copied = copy(input);
        consume(copied);
        return observe_validity(copied);
    };

    const auto actual = bg::testing::test_graph(graph, {4.0F, literal::INVALID, 6.0F});

    CHECK(actual.equals({true, false, true}));
    CHECK(copy_evaluations == 2);
    CHECK(consumed_values == std::vector<float>{4.0F, 6.0F});
}

TEST_CASE("required-input invalidation is applied once while the input remains invalid",
          "[evaluation][invalidation][feature:invalidation][validity:required][transition:invalidation]"
          "[series:value][scalar:floating-point]") {
    const auto fallback = bg::node<fallback_definition>();
    fallback_definition::evaluation_count = 0;
    auto source_evaluations = 0;
    const auto counted_source = bg::node([&](bg::In<bg::TS<bool>> valid, bg::Out<bg::TS<float>> output) {
        ++source_evaluations;
        if (*valid) {
            output.set(4.0F);
        } else {
            output.invalidate();
        }
    });
    const auto repeated_graph = [&](bg::wp<bg::TS<bool>> valid) { return fallback(counted_source(valid)); };
    const auto actual = bg::testing::test_graph(repeated_graph, {true, false, false});

    CHECK(actual.equals({4.0F, -1.0F, std::nullopt}));
    CHECK(source_evaluations == 3);
    CHECK(fallback_definition::evaluation_count == 1);
}

namespace {

struct hold_definition {
    static constexpr auto on_invalidation = bg::preserve_output;

    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("a node can preserve its output when a required input becomes invalid",
          "[evaluation][invalidation][feature:invalidation][validity:required][series:value]"
          "[scalar:floating-point]") {
    const auto hold = bg::node<hold_definition>();
    const auto observe = bg::node([](bg::In<bg::TS<float>, bg::optional> input,
                                     bg::In<bg::TS<bg::i64>>,
                                     bg::Out<bg::TS<float>> output) {
        if (input.valid()) {
            output.set(*input);
        } else {
            output.invalidate();
        }
    });
    auto downstream_evaluations = 0;
    const auto downstream = bg::node([&](bg::In<bg::TS<float>>) { ++downstream_evaluations; });
    const auto graph = [&](bg::wp<bg::TS<float>> input, bg::wp<bg::TS<bg::i64>> tick) {
        const auto held = hold(input);
        downstream(held);
        return observe(held, tick);
    };

    const auto actual = bg::testing::test_graph(graph, {4.0F, literal::INVALID}, {0, 1});

    CHECK(actual.equals({4.0F, 4.0F}));
    CHECK(downstream_evaluations == 1);
}

TEST_CASE("a node can produce an output when a required input becomes invalid",
          "[evaluation][invalidation][feature:invalidation][validity:required][series:value]"
          "[scalar:floating-point]") {
    const auto fallback = bg::node<fallback_definition>();
    const auto graph = [&](bg::wp<bg::TS<float>> input) { return fallback(input); };
    fallback_definition::evaluation_count = 0;

    const auto actual = bg::testing::test_graph(graph, {4.0F, literal::INVALID});

    CHECK(actual.equals({4.0F, -1.0F}));
    CHECK(fallback_definition::evaluation_count == 1);
}

namespace {

struct switchable_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        if (publish) {
            output.set(4.0F);
        } else {
            output.invalidate();
        }
    }

    inline static bool publish{true};
};

struct throwing_invalidation_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {}
    void on_invalidation() const { throw std::runtime_error{"invalidation failure"}; }
    void stop() const noexcept { ++stop_count; }

    inline static int stop_count{};
};

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("an invalidation failure stops the runtime graph",
          "[evaluation][invalidation][lifecycle][feature:invalidation][transition:invalidation]"
          "[transition:stop][series:value][scalar:floating-point]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto throwing_invalidation = bg::node<throwing_invalidation_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        throwing_invalidation(switchable());
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    switchable_definition::publish = true;
    throwing_invalidation_definition::stop_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    switchable_definition::publish = false;
    runtime.schedule(0);
    CHECK_THROWS_AS(runtime.evaluate(time(2)), std::runtime_error);
    CHECK(throwing_invalidation_definition::stop_count == 1);
    CHECK_THROWS_AS(runtime.evaluate(time(3)), std::logic_error);
}
