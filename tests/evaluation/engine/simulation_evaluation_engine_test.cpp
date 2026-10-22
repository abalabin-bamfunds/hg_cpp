#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace bg;

namespace {

struct scheduled_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(EvaluationContext *context) const {
        evaluation_times.push_back(context->evaluation_time());
        if (evaluation_delay != std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(evaluation_delay);
        }
        output.set(static_cast<int>(evaluation_times.size()) + 1);

        if (evaluation_times.size() == 1) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{3});
        }
    }

    void stop() const noexcept { ++stop_count; }

    inline static std::vector<EvaluationTime> evaluation_times;
    inline static std::size_t stop_count{};
    inline static std::chrono::milliseconds evaluation_delay{};
};

struct int_sink_definition {
    In<TS<i64>> input;

    void evaluate() const { values.push_back(*input); }

    inline static std::vector<int> values;
};

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a simulation evaluation engine runs until timed scheduling is empty",
          "[evaluation][simulation][feature:simulation][series:value][scalar:integer]") {
    const auto scheduled_source = bg::node<scheduled_source_definition>();

    const auto int_sink = bg::node<int_sink_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        int_sink(scheduled_source());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    scheduled_source_definition::evaluation_times.clear();
    scheduled_source_definition::stop_count = 0;
    scheduled_source_definition::evaluation_delay = std::chrono::milliseconds{};
    int_sink_definition::values.clear();

    SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(scheduled_source_definition::evaluation_times == std::vector{time(3), time(6)});
    CHECK(int_sink_definition::values == std::vector{1, 2, 3});
    CHECK(scheduled_source_definition::stop_count == 1);
}

namespace {

struct unordered_schedule_definition {
    static constexpr bool uses_timed_scheduling = true;

    int value;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{9});
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{5});
    }

    void evaluate(EvaluationContext *context) const { evaluation_times.push_back(context->evaluation_time()); }

    inline static std::vector<EvaluationTime> evaluation_times;
};

} // namespace

TEST_CASE("timed scheduling orders requests by evaluation time",
          "[evaluation][simulation][feature:simulation][series:value][scalar:integer]") {
    const auto unordered_schedule = bg::node<unordered_schedule_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        unordered_schedule(0);
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    unordered_schedule_definition::evaluation_times.clear();

    SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(unordered_schedule_definition::evaluation_times == std::vector{time(3), time(6), time(10)});
}

TEST_CASE("simulation periodic callback failure at termination stops a started graph",
          "[evaluation][simulation][feature:simulation][feature:lifecycle][failure]") {
    const auto scheduled_source = bg::node<scheduled_source_definition>();

    const auto int_sink = bg::node<int_sink_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        int_sink(scheduled_source());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    scheduled_source_definition::evaluation_times.clear();
    scheduled_source_definition::stop_count = 0;
    scheduled_source_definition::evaluation_delay = std::chrono::milliseconds{2};
    int_sink_definition::values.clear();

    auto callback_calls = std::size_t{};
    auto periodic_callback = PeriodicCallback{
        .callback =
            [&] {
                ++callback_calls;
                if (!scheduled_source_definition::evaluation_times.empty()) {
                    throw std::runtime_error{"periodic callback failed"};
                }
            },
        .interval = std::chrono::milliseconds{1},
    };

    CHECK_THROWS_AS(SimulationEvaluationEngine{}.run(runtime, time(1), periodic_callback), std::runtime_error);
    CHECK(callback_calls >= 2);
    CHECK(scheduled_source_definition::evaluation_times == std::vector{time(3), time(6)});
    CHECK(int_sink_definition::values == std::vector{1, 2, 3});
    CHECK(scheduled_source_definition::stop_count == 1);
    CHECK(runtime.evaluation_time() == time(6));
    scheduled_source_definition::evaluation_delay = std::chrono::milliseconds{};
}

namespace {

struct periodic_callback_chain_definition {
    static constexpr bool uses_timed_scheduling = true;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        ++evaluation_count;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (evaluation_count < 20) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void stop() const noexcept { ++stop_count; }

    inline static std::size_t evaluation_count{};
    inline static std::size_t stop_count{};
};

} // namespace

TEST_CASE("simulation polls periodic callbacks after eight completed cycles",
          "[evaluation][simulation][feature:simulation][feature:lifecycle][failure]") {
    const auto source = bg::node<periodic_callback_chain_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    periodic_callback_chain_definition::evaluation_count = 0;
    periodic_callback_chain_definition::stop_count = 0;
    auto callback_calls = std::size_t{};
    const auto periodic_callback = PeriodicCallback{
        .callback =
            [&] {
                if (++callback_calls == 2) {
                    throw std::runtime_error{"periodic callback failed"};
                }
            },
        .interval = std::chrono::milliseconds{1},
    };

    CHECK_THROWS_AS(SimulationEvaluationEngine{}.run(runtime, time(1), periodic_callback), std::runtime_error);
    CHECK(callback_calls == 2);
    // The initial outer cycle has no timer firing, so seven source evaluations complete eight cycles.
    CHECK(periodic_callback_chain_definition::evaluation_count == 7);
    CHECK(periodic_callback_chain_definition::stop_count == 1);
    CHECK(runtime.evaluation_time() == time(8));
}

TEST_CASE("simulation validates an enabled periodic callback interval before starting",
          "[evaluation][simulation][feature:simulation][failure]") {
    const auto scheduled_source = bg::node<scheduled_source_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)scheduled_source();
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    scheduled_source_definition::evaluation_times.clear();
    scheduled_source_definition::stop_count = 0;

    const auto periodic_callback = PeriodicCallback{
        .callback = [] {},
        .interval = std::chrono::steady_clock::duration::zero(),
    };
    CHECK_THROWS_AS(SimulationEvaluationEngine{}.run(runtime, time(1), periodic_callback), std::invalid_argument);
    CHECK(scheduled_source_definition::evaluation_times.empty());
    CHECK(scheduled_source_definition::stop_count == 0);
}
