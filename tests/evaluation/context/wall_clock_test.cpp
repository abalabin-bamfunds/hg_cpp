#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace {

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct observed_wall_clock {
    void observe(bg::EvaluationContext *context) noexcept {
        const auto *current = &context->wall_clock();
        if (first == nullptr) {
            first = current;
            return;
        }

        matches = matches && first == current;
    }

    const bg::WallClock *first{};
    bool matches{true};
};

struct lifecycle_definition {
    void start(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[1] = context->wall_clock().now();
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[2] = context->wall_clock().now();
    }

    inline static std::array<bg::EvaluationTime, 3> observations{};
    inline static observed_wall_clock identity{};
};

struct scheduled_lifecycle_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        context->schedule_now();
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[++evaluation_count] = context->wall_clock().now();
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[3] = context->wall_clock().now();
    }

    inline static std::array<bg::EvaluationTime, 4> observations{};
    inline static std::size_t evaluation_count{};
    inline static observed_wall_clock identity{};
};

struct failing_lifecycle_definition {
    void start(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[1] = context->wall_clock().now();
        throw std::runtime_error{"evaluation failure"};
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[2] = context->wall_clock().now();
    }

    inline static std::array<bg::EvaluationTime, 3> observations{};
    inline static observed_wall_clock identity{};
};

struct invalidating_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        clock = &context->wall_clock();
        output.invalidate();
    }

    inline static const bg::WallClock *clock{};
};

struct invalidation_observer_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const {}

    void on_invalidation(bg::EvaluationContext *context) const {
        observed_time = context->wall_clock().now();
        shares_clock = &context->wall_clock() == invalidating_source_definition::clock;
    }

    inline static bg::EvaluationTime observed_time{bg::MIN_DT};
    inline static bool shares_clock{};
};

struct nested_lifecycle_definition {
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        output.set(1);
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        identity.observe(context);
        observations[1] = context->wall_clock().now();
        output.set(2);
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[2] = context->wall_clock().now();
    }

    inline static std::array<bg::EvaluationTime, 3> observations{};
    inline static observed_wall_clock identity{};
};

struct nested_run_source_definition {
    bg::Out<bg::TS<bool>> output;

    void start() const { output.set(true); }
};

} // namespace

TEST_CASE("the simulation engine shares one wall clock across node contexts",
          "[evaluation][simulation][context][feature:wall-clock][transition:start][transition:stop]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        lifecycle();
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    lifecycle_definition::observations.fill(bg::MIN_DT);
    lifecycle_definition::identity = {};

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(lifecycle_definition::observations == std::array{time(1), time(1), time(1)});
    CHECK(lifecycle_definition::identity.matches);
    lifecycle_definition::identity = {};
}

TEST_CASE("direct runtime graph lifecycle paths use the system wall clock",
          "[evaluation][context][feature:wall-clock][transition:start][transition:stop]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        lifecycle();
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    const auto before = std::chrono::time_point_cast<bg::EvaluationTime::duration>(std::chrono::system_clock::now());
    lifecycle_definition::observations.fill(bg::MIN_DT);
    lifecycle_definition::identity = {};

    runtime.start(time(1));
    runtime.evaluate(time(2));
    runtime.stop();

    const auto after = std::chrono::time_point_cast<bg::EvaluationTime::duration>(std::chrono::system_clock::now());
    for (const auto observed : lifecycle_definition::observations) {
        CHECK(observed >= before);
        CHECK(observed <= after);
    }
    CHECK(lifecycle_definition::identity.matches);
    lifecycle_definition::identity = {};
}

TEST_CASE("one simulation wall clock follows each outer evaluation cycle",
          "[evaluation][simulation][context][feature:wall-clock][transition:lifecycle]") {
    const auto lifecycle = bg::node<scheduled_lifecycle_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        lifecycle();
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    scheduled_lifecycle_definition::observations.fill(bg::MIN_DT);
    scheduled_lifecycle_definition::evaluation_count = 0;
    scheduled_lifecycle_definition::identity = {};

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(scheduled_lifecycle_definition::observations[0] == time(1));
    CHECK(scheduled_lifecycle_definition::observations[1] == time(1));
    CHECK(scheduled_lifecycle_definition::observations[2] == time(2));
    CHECK(scheduled_lifecycle_definition::observations[3] == time(2));
    CHECK(scheduled_lifecycle_definition::identity.matches);
    scheduled_lifecycle_definition::identity = {};
}

TEST_CASE("failure stop functions retain the active wall clock",
          "[evaluation][simulation][lifecycle][failure][feature:wall-clock][transition:stop]") {
    const auto failing = bg::node<failing_lifecycle_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        failing();
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    failing_lifecycle_definition::observations.fill(bg::MIN_DT);
    failing_lifecycle_definition::identity = {};

    CHECK_THROWS_AS(bg::SimulationEvaluationEngine{}.run(runtime, time(1)), std::runtime_error);

    CHECK(failing_lifecycle_definition::observations == std::array{time(1), time(1), time(1)});
    CHECK(failing_lifecycle_definition::identity.matches);
    failing_lifecycle_definition::identity = {};
}

TEST_CASE("invalidation functions retain the active wall clock",
          "[evaluation][simulation][invalidation][feature:wall-clock][transition:invalidation]") {
    const auto source = bg::node<invalidating_source_definition>();
    const auto observer = bg::node<invalidation_observer_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        observer(source());
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    invalidation_observer_definition::observed_time = bg::MIN_DT;
    invalidating_source_definition::clock = nullptr;
    invalidation_observer_definition::shares_clock = false;

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(invalidation_observer_definition::observed_time == time(2));
    CHECK(invalidation_observer_definition::shares_clock);
    invalidating_source_definition::clock = nullptr;
}

TEST_CASE("nested graph contexts share the outer simulation wall clock",
          "[evaluation][simulation][nested][context][feature:wall-clock]") {
    using namespace bg::literals;

    const auto lifecycle = bg::node<nested_lifecycle_definition>();
    const auto inner =
        bg::graph("name"_n = "inner_wall_clock", [&]() -> bg::wp<bg::TS<bg::i64>> { return lifecycle(); });
    const auto outer =
        bg::graph("name"_n = "outer_wall_clock", [&](bg::wp<bg::TS<bool>> run) { return bg::nested(inner, run); });
    const auto run_source = bg::node<nested_run_source_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)outer(run_source());
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    nested_lifecycle_definition::observations.fill(bg::MIN_DT);
    nested_lifecycle_definition::identity = {};

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(nested_lifecycle_definition::observations == std::array{time(1), time(1), time(1)});
    CHECK(nested_lifecycle_definition::identity.matches);
    nested_lifecycle_definition::identity = {};
}
