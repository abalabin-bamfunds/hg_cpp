#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace {

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
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

    const auto unscheduled = bg::CompiledGraph{unscheduled_wiring};
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

TEST_CASE("a runtime graph activates timed scheduling at the start of evaluation",
          "[evaluation][timed_scheduling][feature:timed-scheduling][transition:start]") {
    const auto cancelling_source = bg::node<cancelling_source_definition>();
    const auto int_sink = bg::node<int_sink_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        int_sink(cancelling_source());
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
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

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
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

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
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

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
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

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};

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

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};

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

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.stop();

    CHECK_FALSE(runtime.next_scheduled_time().has_value());
}

} // namespace
