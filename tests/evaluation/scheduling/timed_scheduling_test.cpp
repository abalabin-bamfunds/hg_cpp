#include <bg/graph/compiled_graph.hpp>
#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/wall_clock.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace {

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct unscheduled_node_definition {
    int value;

    void evaluate() const {}
};

struct scheduling_node_definition {
    static constexpr bool uses_timed_scheduling = true;

    int value;

    void evaluate() const {}
};

TEST_CASE("timed scheduling adds storage only to graphs that declare it",
          "[evaluation][timed_scheduling][feature:timed-scheduling][transition:addition]") {
    const auto unscheduled_node = bg::node<unscheduled_node_definition>();
    const auto scheduling_node = bg::node<scheduling_node_definition>();
    auto unscheduled_wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{unscheduled_wiring};
        unscheduled_node(0);
    }

    auto scheduling_wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{scheduling_wiring};
        scheduling_node(0);
    }

    unscheduled_wiring.finalize();
    const auto unscheduled = bg::CompiledGraph{unscheduled_wiring};
    scheduling_wiring.finalize();
    const auto scheduling = bg::CompiledGraph{scheduling_wiring};

    CHECK(scheduling.runtime_storage_size() == unscheduled.runtime_storage_size() + sizeof(void *));
}

struct cancelling_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        const auto cancelled = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        context->cancel(cancelled);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(bg::EvaluationContext *context) const {
        evaluation_time = context->evaluation_time();
        output.set(1);
    }

    inline static bg::EvaluationTime evaluation_time{bg::MIN_DT};
};

struct int_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const {}
};

struct simulation_timer_progression_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        context->schedule_now();
        context->schedule_at(evaluation_deadline);
        if (!schedule_wall_clock_during_evaluation) {
            context->schedule_wall_clock_at(wall_clock_deadline);
        }
    }

    void evaluate(bg::EvaluationContext *context) const {
        evaluation_times[evaluation_count++] = context->evaluation_time();
        if (schedule_wall_clock_during_evaluation && context->evaluation_time() == time(1)) {
            context->schedule_wall_clock_at(wall_clock_deadline);
        }
    }

    inline static bool schedule_wall_clock_during_evaluation{};
    inline static bg::EvaluationTime evaluation_deadline{bg::MIN_DT};
    inline static bg::EvaluationTime wall_clock_deadline{bg::MIN_DT};
    inline static std::array<bg::EvaluationTime, 4> evaluation_times{};
    inline static std::size_t evaluation_count{};
};

struct nested_pending_timer_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> attempts;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        initial_handle = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    bool evaluate(bg::EvaluationContext *context) const {
        ++*attempts;
        if (*attempts == 1) {
            fired_during_pending = context->timer_fired(initial_handle);
            output.set(*attempts);
            return false;
        }
        if (*attempts == 2) {
            fired_after_resume = context->timer_fired(initial_handle);
            output.set(*attempts);
            resumed_handle = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return true;
        }

        initial_handle_retired = !context->timer_fired(initial_handle);
        resumed_timer_fired = context->timer_fired(resumed_handle);
        output.set(*attempts);
        return true;
    }

    inline static bg::ScheduleHandle initial_handle;
    inline static bg::ScheduleHandle resumed_handle;
    inline static bool fired_during_pending{};
    inline static bool fired_after_resume{};
    inline static bool initial_handle_retired{};
    inline static bool resumed_timer_fired{};
};

struct nested_pending_controller_state {
    static constexpr bool supports_byte_copy_initialization = true;

    bg::RuntimeGraphEvaluator evaluator;
    bg::RuntimeGraph *runtime_graph{};
};

struct nested_pending_controller_definition {
    bg::State<nested_pending_controller_state> state;

    void start(bg::EvaluationContext *context) const {
        auto &parent = context->runtime_graph();
        state->evaluator.bind_owner(parent, context->node_index());
        state->runtime_graph = parent.allocation().new_object<bg::RuntimeGraph>(
            *inner_graph, parent.allocation(), state->evaluator, 0);
        auto inner_context = context->child(*state->runtime_graph, nullptr);
        state->runtime_graph->start(inner_context);
    }

    bool evaluate(bg::EvaluationContext *context) const {
        auto inner_context = context->child(*state->runtime_graph, nullptr);
        if (state->runtime_graph->evaluate(inner_context)) {
            return true;
        }
        saw_pending = true;
        resumed = state->runtime_graph->evaluate(inner_context);
        return resumed;
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        if (state->runtime_graph == nullptr) {
            return;
        }
        auto inner_context = context->child(*state->runtime_graph, nullptr);
        state->runtime_graph->stop(inner_context);
        state->runtime_graph->finalize(inner_context);
        context->runtime_graph().allocation().delete_object(state->runtime_graph);
        state->runtime_graph = nullptr;
    }

    inline static const bg::CompiledGraph *inner_graph{};
    inline static bool saw_pending{};
    inline static bool resumed{};
};

TEST_CASE("fired handles remain visible while a nested graph resumes in one outer cycle",
          "[evaluation][nested][timed_scheduling][feature:nested-graph][feature:timed-scheduling]"
          "[transition:lifecycle]") {
    using namespace bg::literals;

    const auto inner_timer = bg::node<nested_pending_timer_definition>();
    bg::GraphWiring inner_wiring;
    {
        bg::WiringContext context{inner_wiring};
        (void)inner_timer();
    }
    inner_wiring.finalize();
    const bg::CompiledGraph inner_compiled{inner_wiring};
    nested_pending_controller_definition::inner_graph = &inner_compiled;

    const auto controller = bg::node<nested_pending_controller_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        controller();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    nested_pending_timer_definition::fired_during_pending = false;
    nested_pending_timer_definition::fired_after_resume = false;
    nested_pending_timer_definition::initial_handle_retired = false;
    nested_pending_timer_definition::resumed_timer_fired = false;
    nested_pending_controller_definition::saw_pending = false;
    nested_pending_controller_definition::resumed = false;
    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(2));

    runtime.evaluate(time(2));

    CHECK(nested_pending_controller_definition::saw_pending);
    CHECK(nested_pending_controller_definition::resumed);
    CHECK(nested_pending_timer_definition::fired_during_pending);
    CHECK(nested_pending_timer_definition::fired_after_resume);
    REQUIRE(runtime.next_scheduled_time() == time(3));
    runtime.evaluate(time(3));
    CHECK(nested_pending_timer_definition::initial_handle_retired);
    CHECK(nested_pending_timer_definition::resumed_timer_fired);
    runtime.stop();
}

struct indexed_heap_cancellation_definition {
    static constexpr bool uses_timed_scheduling = true;
    static constexpr std::size_t timer_count = 128;

    static constexpr std::size_t deadline_for(std::size_t index) { return (index * 37) % timer_count + 2; }
    static constexpr bool is_cancelled(std::size_t index) { return (index * 13) % 17 < 6; }

    void start(bg::EvaluationContext *context) const {
        for (std::size_t index = 0; index < timer_count; ++index) {
            handles[index] = context->schedule_at(time(deadline_for(index)));
        }

        for (std::size_t index = 0; index < timer_count; ++index) {
            if (is_cancelled(index)) {
                context->cancel(handles[index]);
            }
        }
    }

    void evaluate(bg::EvaluationContext *context) const {
        for (std::size_t index = 0; index < timer_count; ++index) {
            if (!is_cancelled(index) && time(deadline_for(index)) == context->evaluation_time()) {
                fired_in_order[observed_count] = context->timer_fired(handles[index]);
                observed_deadlines[observed_count] = deadline_for(index);
                ++observed_count;
                return;
            }
        }
    }

    inline static std::array<bg::ScheduleHandle, timer_count> handles{};
    inline static std::array<std::size_t, timer_count> observed_deadlines{};
    inline static std::array<bool, timer_count> fired_in_order{};
    inline static std::size_t observed_count{};
};

TEST_CASE("arbitrary timer cancellations preserve heap order",
          "[evaluation][timed_scheduling][feature:timed-scheduling][transition:removal]") {
    const auto node = bg::node<indexed_heap_cancellation_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    indexed_heap_cancellation_definition::observed_count = 0;
    runtime.start(time(1));
    runtime.evaluate(time(1));

    std::size_t expected_count{};
    for (std::size_t deadline = 2; deadline < indexed_heap_cancellation_definition::timer_count + 2; ++deadline) {
        const auto index = ((deadline - 2) * 45) % indexed_heap_cancellation_definition::timer_count;
        if (indexed_heap_cancellation_definition::is_cancelled(index)) {
            continue;
        }

        const auto next_time = runtime.next_scheduled_time();
        REQUIRE(next_time == time(deadline));
        runtime.evaluate(*next_time);
        CHECK(indexed_heap_cancellation_definition::fired_in_order[expected_count]);
        CHECK(indexed_heap_cancellation_definition::observed_deadlines[expected_count] ==
              deadline);
        ++expected_count;
    }

    CHECK(indexed_heap_cancellation_definition::observed_count == expected_count);
    CHECK_FALSE(runtime.next_scheduled_time());
    runtime.stop();
}

struct fired_timer_lifecycle_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        original_handle = context->schedule_at(time(2));
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (context->evaluation_time() == time(2)) {
            fired_before_cancel = context->timer_fired(original_handle);
            context->cancel(original_handle);
            fired_after_cancel = context->timer_fired(original_handle);
            replacement_handle = context->schedule_at(time(3));
            return;
        }

        if (context->evaluation_time() == time(3)) {
            original_retired = !context->timer_fired(original_handle);
            replacement_fired = context->timer_fired(replacement_handle);
            reused_slot_handle = context->schedule_at(time(4));
            reused_slot_same_as_original = reused_slot_handle.slot == original_handle.slot;
            reused_slot_has_new_id = reused_slot_handle.id != original_handle.id;
            stale_handle_ignored = !context->timer_fired(original_handle);
            context->cancel(original_handle);
        }

        if (context->evaluation_time() == time(4)) {
            reused_slot_fired = context->timer_fired(reused_slot_handle);
            original_still_stale = !context->timer_fired(original_handle);
        }
    }

    inline static bg::ScheduleHandle original_handle;
    inline static bg::ScheduleHandle replacement_handle;
    inline static bg::ScheduleHandle reused_slot_handle;
    inline static bool fired_before_cancel{};
    inline static bool fired_after_cancel{};
    inline static bool original_retired{};
    inline static bool replacement_fired{};
    inline static bool stale_handle_ignored{};
    inline static bool reused_slot_same_as_original{};
    inline static bool reused_slot_has_new_id{};
    inline static bool reused_slot_fired{};
    inline static bool original_still_stale{};
};

TEST_CASE("fired handles survive cancellation and become stale after the next cycle",
          "[evaluation][timed_scheduling][feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<fired_timer_lifecycle_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    fired_timer_lifecycle_definition::fired_before_cancel = false;
    fired_timer_lifecycle_definition::fired_after_cancel = false;
    fired_timer_lifecycle_definition::original_retired = false;
    fired_timer_lifecycle_definition::replacement_fired = false;
    fired_timer_lifecycle_definition::stale_handle_ignored = false;
    fired_timer_lifecycle_definition::reused_slot_same_as_original = false;
    fired_timer_lifecycle_definition::reused_slot_has_new_id = false;
    fired_timer_lifecycle_definition::reused_slot_fired = false;
    fired_timer_lifecycle_definition::original_still_stale = false;
    runtime.start(time(1));
    runtime.evaluate(time(1));
    runtime.evaluate(time(2));

    CHECK(fired_timer_lifecycle_definition::fired_before_cancel);
    CHECK(fired_timer_lifecycle_definition::fired_after_cancel);
    runtime.evaluate(time(3));
    CHECK(fired_timer_lifecycle_definition::original_retired);
    CHECK(fired_timer_lifecycle_definition::replacement_fired);
    CHECK(fired_timer_lifecycle_definition::reused_slot_same_as_original);
    CHECK(fired_timer_lifecycle_definition::reused_slot_has_new_id);
    CHECK(fired_timer_lifecycle_definition::stale_handle_ignored);
    runtime.evaluate(time(4));

    CHECK(fired_timer_lifecycle_definition::reused_slot_fired);
    CHECK(fired_timer_lifecycle_definition::original_still_stale);
    runtime.stop();
}

TEST_CASE("simulation advances to mixed evaluation and wall-clock deadlines",
          "[evaluation][simulation][timed_scheduling][feature:simulation][feature:wall-clock]"
          "[feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<simulation_timer_progression_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    simulation_timer_progression_definition::evaluation_deadline = time(10);
    simulation_timer_progression_definition::wall_clock_deadline = time(5);
    simulation_timer_progression_definition::schedule_wall_clock_during_evaluation = false;
    simulation_timer_progression_definition::evaluation_count = 0;

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    REQUIRE(simulation_timer_progression_definition::evaluation_count == 3);
    CHECK(simulation_timer_progression_definition::evaluation_times == std::array{time(1), time(5), time(10),
                                                                                  bg::MIN_DT});
}

TEST_CASE("simulation advances overdue wall-clock timers at the next minimum time",
          "[evaluation][simulation][timed_scheduling][feature:simulation][feature:wall-clock]"
          "[feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<simulation_timer_progression_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    simulation_timer_progression_definition::evaluation_deadline = time(7);
    simulation_timer_progression_definition::wall_clock_deadline = time(0);
    simulation_timer_progression_definition::schedule_wall_clock_during_evaluation = true;
    simulation_timer_progression_definition::evaluation_count = 0;

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    REQUIRE(simulation_timer_progression_definition::evaluation_count == 3);
    CHECK(simulation_timer_progression_definition::evaluation_times == std::array{time(1), time(2), time(7),
                                                                                  bg::MIN_DT});
}

struct wall_clock_readiness_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        deadline = bg::SystemWallClock::instance().now() + std::chrono::hours{1};
        handle = context->schedule_wall_clock_at(deadline);
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        fired = context->timer_fired(handle);
        evaluation_time = context->evaluation_time();
    }

    inline static bg::EvaluationTime deadline{bg::MIN_DT};
    inline static bg::EvaluationTime evaluation_time{bg::MIN_DT};
    inline static bg::ScheduleHandle handle;
    inline static bool fired{};
};

TEST_CASE("wall-clock readiness does not follow evaluation time",
          "[evaluation][timed_scheduling][feature:wall-clock][feature:timed-scheduling]"
          "[transition:lifecycle]") {
    const auto node = bg::node<wall_clock_readiness_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    wall_clock_readiness_definition::fired = false;
    runtime.start(time(1));
    const auto evaluation_time = wall_clock_readiness_definition::deadline + std::chrono::hours{1};

    runtime.evaluate(evaluation_time);

    CHECK(wall_clock_readiness_definition::evaluation_time == evaluation_time);
    CHECK_FALSE(wall_clock_readiness_definition::fired);
    CHECK(runtime.next_wall_clock_time() == wall_clock_readiness_definition::deadline);
    runtime.stop();
}

struct overdue_wall_clock_coalescing_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        for (auto index = std::size_t{}; index != handles.size(); ++index) {
            handles[index] = context->schedule_wall_clock_at(time(static_cast<bg::i64>(index)));
        }
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluation_count;
        for (const auto handle : handles) {
            all_fired = all_fired && context->timer_fired(handle);
        }
    }

    inline static std::array<bg::ScheduleHandle, 3> handles{};
    inline static int evaluation_count{};
    inline static bool all_fired{true};
};

TEST_CASE("overdue wall-clock timers coalesce in one outer cycle",
          "[evaluation][timed_scheduling][feature:wall-clock][feature:timed-scheduling]"
          "[transition:lifecycle]") {
    const auto node = bg::node<overdue_wall_clock_coalescing_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    overdue_wall_clock_coalescing_definition::evaluation_count = 0;
    overdue_wall_clock_coalescing_definition::all_fired = true;
    runtime.start(time(1));

    runtime.evaluate(time(1));

    CHECK(overdue_wall_clock_coalescing_definition::evaluation_count == 1);
    CHECK(overdue_wall_clock_coalescing_definition::all_fired);
    CHECK_FALSE(runtime.next_wall_clock_time());
    runtime.stop();
}

TEST_CASE("a runtime graph activates timed scheduling at the start of evaluation",
          "[evaluation][timed_scheduling][feature:timed-scheduling][transition:start]") {
    const auto cancelling_source = bg::node<cancelling_source_definition>();
    const auto int_sink = bg::node<int_sink_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        int_sink(cancelling_source());
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    cancelling_source_definition::evaluation_time = bg::MIN_DT;

    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.evaluate(time(3));

    CHECK(cancelling_source_definition::evaluation_time == time(3));
    CHECK_FALSE(runtime.next_scheduled_time().has_value());
}

struct coalescing_node_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        const auto scheduled_time = context->evaluation_time() + std::chrono::microseconds{2};
        context->schedule_at(scheduled_time);
        context->schedule_at(scheduled_time);
    }

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

TEST_CASE("schedule requests for one node at the same time coalesce",
          "[evaluation][timed_scheduling][feature:timed-scheduling]") {
    const auto coalescing_node = bg::node<coalescing_node_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        coalescing_node();
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    coalescing_node_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.evaluate(time(3));

    CHECK(coalescing_node_definition::evaluation_count == 1);
    CHECK_FALSE(runtime.next_scheduled_time().has_value());
}

struct delayed_node_definition {
    static constexpr bool uses_timed_scheduling = true;

    int id;
    int delay;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{delay});
    }

    void evaluate() const { ++evaluation_counts[id]; }

    inline static std::array<int, 2> evaluation_counts{};
};

TEST_CASE("evaluation activates every overdue schedule request",
          "[evaluation][timed_scheduling][feature:timed-scheduling]") {
    const auto delayed_node = bg::node<delayed_node_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        delayed_node(0, 2);
        delayed_node(1, 6);
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    delayed_node_definition::evaluation_counts = {0, 0};

    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.evaluate(time(10));

    CHECK(delayed_node_definition::evaluation_counts == std::array{1, 1});
    CHECK_FALSE(runtime.next_scheduled_time().has_value());
}

struct schedule_owner_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        handle = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const { ++evaluation_count; }

    inline static bg::ScheduleHandle handle;
    inline static int evaluation_count{};
};

struct foreign_canceller_definition {
    void start(bg::EvaluationContext *context) const { context->cancel(schedule_owner_definition::handle); }

    void evaluate() const {}
};

TEST_CASE("a schedule handle identifies the node that created it",
          "[evaluation][timed_scheduling][feature:timed-scheduling]") {
    const auto schedule_owner = bg::node<schedule_owner_definition>();
    const auto foreign_canceller = bg::node<foreign_canceller_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        schedule_owner();
        foreign_canceller();
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    schedule_owner_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.evaluate(time(3));

    CHECK(schedule_owner_definition::evaluation_count == 1);
}

struct invalid_time_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const { context->schedule_at(context->evaluation_time()); }

    void evaluate() const {}
};

TEST_CASE("timed scheduling requires a time after the current outer cycle",
          "[evaluation][timed_scheduling][feature:timed-scheduling]") {
    const auto invalid_time = bg::node<invalid_time_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        invalid_time();
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};

    CHECK_THROWS_AS(runtime.start(time(1)), std::invalid_argument);
}

struct undeclared_scheduling_definition {
    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {}
};

TEST_CASE("a node must declare timed scheduling", "[evaluation][timed_scheduling][feature:timed-scheduling]") {
    const auto undeclared_scheduling = bg::node<undeclared_scheduling_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        undeclared_scheduling();
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};

    CHECK_THROWS_AS(runtime.start(time(1)), std::logic_error);
}

TEST_CASE("stopping a runtime graph clears schedule requests",
          "[evaluation][timed_scheduling][lifecycle][feature:timed-scheduling][transition:removal]"
          "[transition:stop]") {
    const auto coalescing_node = bg::node<coalescing_node_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        coalescing_node();
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.stop();

    CHECK_FALSE(runtime.next_scheduled_time().has_value());
}

struct startup_failure_with_timer_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        throw std::runtime_error{"startup failed"};
    }

    void evaluate() const {}
};

TEST_CASE("startup failure clears timers registered before the failure",
          "[evaluation][timed_scheduling][failure][feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<startup_failure_with_timer_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};

    CHECK_THROWS_AS(runtime.start(time(1)), std::runtime_error);
    CHECK_FALSE(runtime.next_scheduled_time());
}

struct evaluation_failure_with_timer_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{5});
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        throw std::runtime_error{"evaluation failed"};
    }
};

TEST_CASE("evaluation failure clears timers registered before the failure",
          "[evaluation][timed_scheduling][failure][feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<evaluation_failure_with_timer_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    runtime.start(time(1));

    CHECK_THROWS_AS(runtime.evaluate(time(1)), std::runtime_error);
    CHECK_FALSE(runtime.next_scheduled_time());
}

struct stop_callback_timer_definition {
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool uses_cycle_callbacks = true;

    stop_callback_timer_definition() : signature_{bg::util::symbol{"stop_callback_timer"}, {}} {}

    const bg::Signature &signature() const noexcept { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        auto &context = invocation.context();
        context.schedule_at(context.evaluation_time() + std::chrono::microseconds{5});
        context.add_cycle_callback(&callback_ran, schedule_timer, true);
    }

    static void evaluate(bg::NodeInvocation<>) {}

    static void schedule_timer(void *receiver, bg::EvaluationContext &context) noexcept {
        *static_cast<bool *>(receiver) = true;
        (void)context.schedule_at(context.evaluation_time() + std::chrono::microseconds{1});
    }

    inline static bool callback_ran{};

  private:
    bg::Signature signature_;
};

struct deferred_cleanup_timer_definition {
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool uses_cycle_callbacks = true;

    deferred_cleanup_timer_definition() : signature_{bg::util::symbol{"deferred_cleanup_timer"}, {}} {}

    const bg::Signature &signature() const noexcept { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        auto &context = invocation.context();
        node_index = context.node_index();
        initial_handle = context.schedule_at(context.evaluation_time() + std::chrono::microseconds{1});
        context.schedule_now();
    }

    static void evaluate(bg::NodeInvocation<> invocation) {
        auto &context = invocation.context();
        if (context.evaluation_time() == time(1)) {
            context.add_cycle_callback(&callback_ran, schedule_timer);
            return;
        }
        if (context.evaluation_time() == time(2)) {
            initial_timer_fired = context.timer_fired(initial_handle);
        }
        if (context.evaluation_time() == time(4)) {
            deferred_timer_fired = context.timer_fired(deferred_handle);
        }
    }

    static void schedule_timer(void *, bg::EvaluationContext &context) noexcept {
        callback_ran = true;
        callback_time = context.evaluation_time();
        try {
            (void)context.schedule_at(node_index, context.evaluation_time());
        } catch (const std::invalid_argument &) {
            current_time_rejected = true;
        } catch (...) {
            registration_failed = true;
        }
        try {
            deferred_handle = context.schedule_at(node_index, context.evaluation_time() + std::chrono::microseconds{2});
        } catch (...) {
            registration_failed = true;
        }
    }

    inline static std::size_t node_index{};
    inline static bg::ScheduleHandle initial_handle;
    inline static bg::ScheduleHandle deferred_handle;
    inline static bg::EvaluationTime callback_time{bg::MIN_DT};
    inline static bool callback_ran{};
    inline static bool current_time_rejected{};
    inline static bool registration_failed{};
    inline static bool initial_timer_fired{};
    inline static bool deferred_timer_fired{};

  private:
    bg::Signature signature_;
};

TEST_CASE("cycle cleanup schedules timers relative to the new invocation time",
          "[evaluation][timed_scheduling][feature:timed-scheduling][transition:lifecycle]") {
    const auto prefix = bg::node<unscheduled_node_definition>();
    const auto node = bg::node(deferred_cleanup_timer_definition{});
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        prefix(0);
        (void)node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    deferred_cleanup_timer_definition::callback_ran = false;
    deferred_cleanup_timer_definition::current_time_rejected = false;
    deferred_cleanup_timer_definition::registration_failed = false;
    deferred_cleanup_timer_definition::initial_timer_fired = false;
    deferred_cleanup_timer_definition::deferred_timer_fired = false;
    deferred_cleanup_timer_definition::callback_time = bg::MIN_DT;
    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(2));

    runtime.evaluate(time(2));

    CHECK(deferred_cleanup_timer_definition::callback_ran);
    CHECK(deferred_cleanup_timer_definition::current_time_rejected);
    CHECK_FALSE(deferred_cleanup_timer_definition::registration_failed);
    CHECK(deferred_cleanup_timer_definition::callback_time == time(2));
    CHECK(deferred_cleanup_timer_definition::initial_timer_fired);
    REQUIRE(runtime.next_scheduled_time() == time(4));
    runtime.evaluate(time(4));

    CHECK(deferred_cleanup_timer_definition::deferred_timer_fired);
    runtime.stop();
}

TEST_CASE("stop callbacks can register timers that stop then clears",
          "[evaluation][timed_scheduling][lifecycle][feature:timed-scheduling][transition:stop]") {
    const auto node = bg::node(stop_callback_timer_definition{});
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        (void)node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    stop_callback_timer_definition::callback_ran = false;
    runtime.start(time(1));

    runtime.stop();

    CHECK(stop_callback_timer_definition::callback_ran);
    CHECK_FALSE(runtime.next_scheduled_time());
}

TEST_CASE("finalizing a running graph clears timers registered by stop callbacks",
          "[evaluation][timed_scheduling][lifecycle][feature:timed-scheduling][feature:finalization]"
          "[transition:lifecycle]") {
    const auto node = bg::node(stop_callback_timer_definition{});
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        (void)node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, global_state};
    stop_callback_timer_definition::callback_ran = false;
    runtime.start(time(1));

    runtime.finalize(global_state);

    CHECK(stop_callback_timer_definition::callback_ran);
    CHECK_FALSE(runtime.next_scheduled_time());
}

struct timer_allocation_failure_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        for (auto index = std::size_t{}; index < initial_timers.size(); ++index) {
            initial_timers[index] = context->schedule_at(
                context->evaluation_time() + std::chrono::microseconds{static_cast<std::int64_t>(index) + 2});
        }
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        resource->fail_after(2);
        try {
            (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{100});
        } catch (const std::bad_alloc &) {
            registration_failed = true;
        }
        resource->allow_allocations();
        retry = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{100});
        context->cancel(retry);
    }

    inline static counting_memory_resource *resource{};
    inline static std::array<bg::ScheduleHandle, 8> initial_timers{};
    inline static bg::ScheduleHandle retry{};
    inline static bool registration_failed{};
};

struct first_timer_allocation_failure_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        resource->fail_after(failure_budget);
        try {
            const auto handle = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
            context->cancel(handle);
        } catch (const std::bad_alloc &) {
            registration_failed = true;
        }
        resource->allow_allocations();

        const auto retry = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{3});
        context->cancel(retry);
    }

    void evaluate() const {}

    inline static counting_memory_resource *resource{};
    inline static std::size_t failure_budget{};
    inline static bool registration_failed{};
};

TEST_CASE("first timer registration rolls back each allocation boundary",
          "[evaluation][timed_scheduling][failure][feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<first_timer_allocation_failure_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};

    for (auto failure_budget = std::size_t{}; failure_budget != 3; ++failure_budget) {
        auto memory = counting_memory_resource{};
        bg::RuntimeGraph runtime{compiled, bg::allocator{memory}, global_state};
        first_timer_allocation_failure_definition::resource = &memory;
        first_timer_allocation_failure_definition::failure_budget = failure_budget;
        first_timer_allocation_failure_definition::registration_failed = false;

        runtime.start(time(1));

        CHECK(first_timer_allocation_failure_definition::registration_failed);
        CHECK_FALSE(runtime.next_scheduled_time());
        runtime.stop();
    }
}

TEST_CASE("failed timer registration rolls back its slot, heap and owner links",
          "[evaluation][timed_scheduling][failure][feature:timed-scheduling][transition:lifecycle]") {
    const auto node = bg::node<timer_allocation_failure_definition>();
    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        node();
    }
    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto global_state = bg::GlobalState{};
    auto memory = counting_memory_resource{};
    bg::RuntimeGraph runtime{compiled, bg::allocator{memory}, global_state};
    timer_allocation_failure_definition::resource = &memory;
    timer_allocation_failure_definition::registration_failed = false;
    timer_allocation_failure_definition::retry = {};
    runtime.start(time(1));

    runtime.evaluate(time(1));

    CHECK(timer_allocation_failure_definition::registration_failed);
    CHECK(timer_allocation_failure_definition::retry);
    CHECK(runtime.next_scheduled_time() == time(3));
    runtime.stop();
}

} // namespace
