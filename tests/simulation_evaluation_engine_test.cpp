#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace {

struct scheduled_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(1);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(bg::EvaluationContext *context) const {
        evaluation_times.push_back(context->evaluation_time());
        output.set(static_cast<int>(evaluation_times.size()) + 1);

        if (evaluation_times.size() == 1) {
            (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{3});
        }
    }

    void stop() const noexcept { ++stop_count; }

    inline static std::vector<bg::EvaluationTime> evaluation_times;
    inline static std::size_t stop_count{};
};

struct int_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { values.push_back(input.value()); }

    inline static std::vector<int> values;
};

struct cancelling_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        const auto cancelled = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        context->cancel(cancelled);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(bg::EvaluationContext *context) const {
        evaluation_time = context->evaluation_time();
        output.set(1);
    }

    inline static bg::EvaluationTime evaluation_time{bg::MIN_DT};
};

struct unscheduled_node_definition {
    int value;

    void evaluate() const {}
};

struct scheduling_node_definition {
    static constexpr bool uses_future_scheduling = true;

    int value;

    void evaluate() const {}
};

struct unordered_schedule_definition {
    static constexpr bool uses_future_scheduling = true;

    int value;

    void start(bg::EvaluationContext *context) const {
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{9});
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{5});
    }

    void evaluate(bg::EvaluationContext *context) const { evaluation_times.push_back(context->evaluation_time()); }

    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

struct global_state_source_definition {
    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        const auto value = bg::ScalarValue{42};
        context->global_state().set(bg::util::symbol{"result"}, value);
        output.set(1);
    }

    void evaluate() const {}
};

inline const auto scheduled_source = bg::node<scheduled_source_definition>();
inline const auto int_sink = bg::node<int_sink_definition>();
inline const auto cancelling_source = bg::node<cancelling_source_definition>();
inline const auto unscheduled_node = bg::node<unscheduled_node_definition>();
inline const auto scheduling_node = bg::node<scheduling_node_definition>();
inline const auto unordered_schedule = bg::node<unordered_schedule_definition>();
inline const auto global_state_source = bg::node<global_state_source_definition>();

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("future scheduling adds storage only to graphs that declare it", "[evaluation][runtime_graph]") {
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

TEST_CASE("a runtime graph activates future scheduling at the start of evaluation", "[evaluation][runtime_graph]") {
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        int_sink(cancelling_source());
    }

    auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    cancelling_source_definition::evaluation_time = bg::MIN_DT;

    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(runtime.next_scheduled_time() == time(3));

    runtime.evaluate(time(3));

    CHECK(cancelling_source_definition::evaluation_time == time(3));
    CHECK_FALSE(runtime.next_scheduled_time().has_value());
}

TEST_CASE("a simulation evaluation engine runs until future scheduling is empty", "[evaluation][simulation]") {
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        int_sink(scheduled_source());
    }

    auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    scheduled_source_definition::evaluation_times.clear();
    scheduled_source_definition::stop_count = 0;
    int_sink_definition::values.clear();

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(scheduled_source_definition::evaluation_times == std::vector{time(3), time(6)});
    CHECK(int_sink_definition::values == std::vector{1, 2, 3});
    CHECK(scheduled_source_definition::stop_count == 1);
}

TEST_CASE("future scheduling orders requests by evaluation time", "[evaluation][simulation]") {
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        unordered_schedule(0);
    }

    auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    unordered_schedule_definition::evaluation_times.clear();

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(unordered_schedule_definition::evaluation_times == std::vector{time(3), time(6), time(10)});
}

TEST_CASE("an evaluation context exposes the outer global state", "[evaluation][global_state]") {
    auto state = bg::GlobalState{};

    bg::run_graph([] { (void)global_state_source(); }, state);

    CHECK(bg::scalar_cast<std::int64_t>(state.at("result"))->value() == 42);
}
