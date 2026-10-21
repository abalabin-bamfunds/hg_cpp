#include <bg/simulation_evaluation_engine.hpp>

#include <bg/details/evaluation_scratch.hpp>
#include <bg/details/periodic_callback_runner.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/evaluation_profile.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <stdexcept>

namespace bg {
namespace {

class SimulationWallClock final : public WallClock {
  public:
    EvaluationTime now() const noexcept override { return time_; }

    void set_time(EvaluationTime time) noexcept { time_ = time; }

  private:
    EvaluationTime time_{MIN_DT};
};

} // namespace

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                     const PeriodicCallback &periodic_callback) const {
    run(runtime_graph, start_time, nullptr, periodic_callback);
}

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                     EvaluationProfile &profile,
                                     const PeriodicCallback &periodic_callback) const {
    run(runtime_graph, start_time, &profile, periodic_callback);
}

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                     EvaluationProfile *profile,
                                     const PeriodicCallback &periodic_callback) const {
    auto callback_runner = details::PeriodicCallbackRunner{periodic_callback};
    callback_runner.run_now();

    auto scratch = details::EvaluationScratch{runtime_graph.allocation().memory_resource()};
    auto wall_clock = SimulationWallClock{};
    wall_clock.set_time(start_time);
    auto global_context = runtime_graph.make_global_evaluation_context(runtime_graph.global_state(), scratch,
                                                                       wall_clock, profile);
    auto start_context = EvaluationContext{global_context, start_time, runtime_graph};
    auto stop_graph = util::scope_exit{[&]() noexcept {
        auto context = EvaluationContext{global_context, runtime_graph.evaluation_time(), runtime_graph};
        runtime_graph.stop(context);
    }};
    runtime_graph.start(start_context);

    const auto evaluate = [&](EvaluationTime time) {
        auto context = EvaluationContext{global_context, time, runtime_graph};
        if (!runtime_graph.evaluate(context)) {
            throw std::logic_error{"a pending evaluation escaped the outer graph"};
        }
    };
    const auto next_scheduled_time = [&]() {
        auto next_time = runtime_graph.next_scheduled_time();
        if (const auto wall_clock_time = runtime_graph.next_wall_clock_time()) {
            const auto next_wall_cycle = std::max(
                *wall_clock_time, runtime_graph.evaluation_time() + EvaluationTime::duration{1});
            if (!next_time || next_wall_cycle < *next_time) {
                next_time = next_wall_cycle;
            }
        }
        return next_time;
    };
    evaluate(start_time);
    callback_runner.poll_after_cycle();

    while (true) {
        auto next_time = next_scheduled_time();
        if (!next_time) {
            callback_runner.run_now();
            next_time = next_scheduled_time();
            if (!next_time) {
                break;
            }
        }

        wall_clock.set_time(*next_time);
        evaluate(*next_time);
        callback_runner.poll_after_cycle();
    }

    const auto stop_time = runtime_graph.evaluation_time();
    auto stop_context = EvaluationContext{global_context, stop_time, runtime_graph};
    runtime_graph.stop(stop_context);
    stop_graph.release();
}

} // namespace bg
