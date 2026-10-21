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

using namespace bg;

namespace {

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

struct observed_wall_clock {
    void observe(EvaluationContext *context) noexcept {
        const auto *current = &context->wall_clock();
        if (first == nullptr) {
            first = current;
            return;
        }

        matches = matches && first == current;
    }

    const WallClock *first{};
    bool matches{true};
};

struct lifecycle_definition {
    void start(EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        context->schedule_now();
    }

    void evaluate(EvaluationContext *context) const {
        identity.observe(context);
        observations[1] = context->wall_clock().now();
    }

    void stop(EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[2] = context->wall_clock().now();
    }

    inline static std::array<EvaluationTime, 3> observations{};
    inline static observed_wall_clock identity{};
};

} // namespace

TEST_CASE("the simulation engine shares one wall clock across node contexts",
          "[evaluation][simulation][context][feature:wall-clock][transition:start][transition:stop]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        lifecycle();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    lifecycle_definition::observations.fill(MIN_DT);
    lifecycle_definition::identity = {};

    SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(lifecycle_definition::observations == std::array{time(1), time(1), time(1)});
    CHECK(lifecycle_definition::identity.matches);
    lifecycle_definition::identity = {};
}

TEST_CASE("direct runtime graph lifecycle paths use the system wall clock",
          "[evaluation][context][feature:wall-clock][transition:start][transition:stop]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        lifecycle();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    const auto before = std::chrono::time_point_cast<EvaluationTime::duration>(std::chrono::system_clock::now());
    lifecycle_definition::observations.fill(MIN_DT);
    lifecycle_definition::identity = {};

    runtime.start(time(1));
    runtime.evaluate(time(2));
    runtime.stop();

    const auto after = std::chrono::time_point_cast<EvaluationTime::duration>(std::chrono::system_clock::now());
    for (const auto observed : lifecycle_definition::observations) {
        CHECK(observed >= before);
        CHECK(observed <= after);
    }
    CHECK(lifecycle_definition::identity.matches);
    lifecycle_definition::identity = {};
}

namespace {

struct scheduled_lifecycle_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        context->schedule_now();
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        identity.observe(context);
        observations[++evaluation_count] = context->wall_clock().now();
    }

    void stop(EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[3] = context->wall_clock().now();
    }

    inline static std::array<EvaluationTime, 4> observations{};
    inline static std::size_t evaluation_count{};
    inline static observed_wall_clock identity{};
};

} // namespace

TEST_CASE("one simulation wall clock follows each outer evaluation cycle",
          "[evaluation][simulation][context][feature:wall-clock][transition:lifecycle]") {
    const auto lifecycle = bg::node<scheduled_lifecycle_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        lifecycle();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    scheduled_lifecycle_definition::observations.fill(MIN_DT);
    scheduled_lifecycle_definition::evaluation_count = 0;
    scheduled_lifecycle_definition::identity = {};

    SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(scheduled_lifecycle_definition::observations[0] == time(1));
    CHECK(scheduled_lifecycle_definition::observations[1] == time(1));
    CHECK(scheduled_lifecycle_definition::observations[2] == time(2));
    CHECK(scheduled_lifecycle_definition::observations[3] == time(2));
    CHECK(scheduled_lifecycle_definition::identity.matches);
    scheduled_lifecycle_definition::identity = {};
}

namespace {

struct failing_lifecycle_definition {
    void start(EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        context->schedule_now();
    }

    void evaluate(EvaluationContext *context) const {
        identity.observe(context);
        observations[1] = context->wall_clock().now();
        throw std::runtime_error{"evaluation failure"};
    }

    void stop(EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[2] = context->wall_clock().now();
    }

    inline static std::array<EvaluationTime, 3> observations{};
    inline static observed_wall_clock identity{};
};

} // namespace

TEST_CASE("failure stop functions retain the active wall clock",
          "[evaluation][simulation][lifecycle][failure][feature:wall-clock][transition:stop]") {
    const auto failing = bg::node<failing_lifecycle_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        failing();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    failing_lifecycle_definition::observations.fill(MIN_DT);
    failing_lifecycle_definition::identity = {};

    CHECK_THROWS_AS(SimulationEvaluationEngine{}.run(runtime, time(1)), std::runtime_error);

    CHECK(failing_lifecycle_definition::observations == std::array{time(1), time(1), time(1)});
    CHECK(failing_lifecycle_definition::identity.matches);
    failing_lifecycle_definition::identity = {};
}

namespace {

struct invalidating_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        clock = &context->wall_clock();
        output.invalidate();
    }

    inline static const WallClock *clock{};
};

struct invalidation_observer_definition {
    In<TS<i64>> input;

    void evaluate() const {}

    void on_invalidation(EvaluationContext *context) const {
        observed_time = context->wall_clock().now();
        shares_clock = &context->wall_clock() == invalidating_source_definition::clock;
    }

    inline static EvaluationTime observed_time{MIN_DT};
    inline static bool shares_clock{};
};

} // namespace

TEST_CASE("invalidation functions retain the active wall clock",
          "[evaluation][simulation][invalidation][feature:wall-clock][transition:invalidation]") {
    const auto source = bg::node<invalidating_source_definition>();
    const auto observer = bg::node<invalidation_observer_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        observer(source());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    invalidation_observer_definition::observed_time = MIN_DT;
    invalidating_source_definition::clock = nullptr;
    invalidation_observer_definition::shares_clock = false;

    SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(invalidation_observer_definition::observed_time == time(2));
    CHECK(invalidation_observer_definition::shares_clock);
    invalidating_source_definition::clock = nullptr;
}

namespace {

struct nested_lifecycle_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        identity.observe(context);
        observations[0] = context->wall_clock().now();
        output.set(1);
        context->schedule_now();
    }

    void evaluate(EvaluationContext *context) const {
        identity.observe(context);
        observations[1] = context->wall_clock().now();
        output.set(2);
    }

    void stop(EvaluationContext *context) const noexcept {
        identity.observe(context);
        observations[2] = context->wall_clock().now();
    }

    inline static std::array<EvaluationTime, 3> observations{};
    inline static observed_wall_clock identity{};
};

} // namespace

TEST_CASE("nested graph contexts share the outer simulation wall clock",
          "[evaluation][simulation][nested][context][feature:wall-clock]") {
    struct nested_run_source_definition {
        Out<TS<bool>> output;

        void start() const { output.set(true); }
    };

    using namespace bg::literals;

    const auto lifecycle = bg::node<nested_lifecycle_definition>();
    const auto inner = bg::graph("name"_n = "inner_wall_clock", [&]() -> bg::wp<TS<i64>> { return lifecycle(); });
    const auto outer =
        bg::graph("name"_n = "outer_wall_clock", [&](bg::wp<TS<bool>> run) { return bg::nested(inner, run); });
    const auto run_source = bg::node<nested_run_source_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)outer(run_source());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    nested_lifecycle_definition::observations.fill(MIN_DT);
    nested_lifecycle_definition::identity = {};

    SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(nested_lifecycle_definition::observations == std::array{time(1), time(1), time(1)});
    CHECK(nested_lifecycle_definition::identity.matches);
    nested_lifecycle_definition::identity = {};
}
