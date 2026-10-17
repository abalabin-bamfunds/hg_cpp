#include <bg/realtime_evaluation_engine.hpp>

#include <bg/details/evaluation_scratch.hpp>
#include <bg/details/engine_wakeup.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/evaluation_profile.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/wall_clock.hpp>

#include <algorithm>
#include <stdexcept>

namespace bg {

void RealtimeEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                   std::stop_token stop_token) const {
    run(runtime_graph, start_time, nullptr, stop_token);
}

void RealtimeEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                   EvaluationProfile &profile, std::stop_token stop_token) const {
    run(runtime_graph, start_time, &profile, stop_token);
}

void RealtimeEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                                   EvaluationProfile *profile, std::stop_token stop_token) const {
    if (stop_token.stop_requested()) {
        return;
    }
    if (!TimeSeriesState::can_represent(start_time)) {
        throw std::invalid_argument{"evaluation time cannot be represented by a time series"};
    }

    auto wakeup = details::EngineWakeup{};
    auto wake_on_stop = std::stop_callback{stop_token, [&wakeup]() noexcept { wakeup.notify(); }};
    const auto &wall_clock = SystemWallClock::instance();
    while (!stop_token.stop_requested() && wall_clock.now() < start_time) {
        wakeup.wait_until(start_time);
    }
    if (stop_token.stop_requested()) {
        return;
    }

    runtime_graph.bind_queue_wakeup(wakeup);
    auto close_queue = util::scope_exit{[&]() noexcept { runtime_graph.close_queue(); }};

    auto scratch = details::EvaluationScratch{runtime_graph.allocation().memory_resource()};
    auto global_context = runtime_graph.make_global_evaluation_context(runtime_graph.global_state(), scratch,
                                                                       wall_clock, profile);
    auto stop_graph = util::scope_exit{[&]() noexcept {
        auto context = EvaluationContext{global_context, runtime_graph.evaluation_time(), runtime_graph};
        runtime_graph.stop(context);
    }};

    auto start_context = EvaluationContext{global_context, start_time, runtime_graph};
    runtime_graph.start(start_context);

    const auto evaluate = [&](EvaluationTime evaluation_time) {
        auto context = EvaluationContext{global_context, evaluation_time, runtime_graph};
        if (!runtime_graph.evaluate(context)) {
            throw std::logic_error{"a pending evaluation escaped the outer graph"};
        }
    };
    evaluate(start_time);

    auto &root_evaluator = *global_context.root_runtime_graph_evaluator;
    while (!stop_token.stop_requested()) {
        const auto now = wall_clock.now();
        const auto exact_time = root_evaluator.next_scheduled_time();
        const auto wall_time = runtime_graph.next_wall_clock_time();
        const auto wall_timer_ready = wall_time && *wall_time <= now;
        const auto external_ready = runtime_graph.has_committed_queue_messages() || wall_timer_ready;

        auto cycle_time = exact_time;
        if (external_ready) {
            const auto external_time =
                std::max(now, runtime_graph.evaluation_time() + EvaluationTime::duration{1});
            if (!cycle_time || external_time < *cycle_time) {
                cycle_time = external_time;
            }
        }

        if (cycle_time && *cycle_time <= now) {
            evaluate(*cycle_time);
            continue;
        }

        auto deadline = cycle_time;
        if (wall_time && *wall_time > now && (!deadline || *wall_time < *deadline)) {
            deadline = wall_time;
        }
        if (deadline) {
            wakeup.wait_until(*deadline);
        } else {
            wakeup.wait();
        }
    }

    auto stop_context = EvaluationContext{global_context, runtime_graph.evaluation_time(), runtime_graph};
    runtime_graph.stop(stop_context);
    stop_graph.release();
    close_queue.release();
}

} // namespace bg
