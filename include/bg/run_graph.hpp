#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/realtime_evaluation_engine.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/wall_clock.hpp>

#include <concepts>
#include <functional>
#include <utility>

namespace bg {

template <std::invocable Function>
void run_graph(Function &&function, const GraphRunOptions &options = {}) {
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        (void)std::invoke(std::forward<Function>(function));
    }

    if (options.services != nullptr) {
        wiring.finalize(*options.services);
    } else {
        wiring.finalize();
    }

    auto compiled = CompiledGraph{wiring};
    const auto default_start_time = options.engine == EvaluationEngine::realtime
                                        ? SystemWallClock::instance().now()
                                        : MIN_DT + EvaluationTime::duration{1};
    const auto start_time = options.start_time.value_or(default_start_time);

    auto run = [&](GlobalState &global_state) {
        auto runtime = RuntimeGraph{compiled, global_state};
        if (options.recorder) {
            options.recorder->run(runtime, start_time, options.engine, options.stop_token, options.interruption);
        } else if (options.engine == EvaluationEngine::realtime) {
            RealtimeEvaluationEngine{}.run(runtime, start_time, options.stop_token, options.interruption);
        } else {
            SimulationEvaluationEngine{}.run(runtime, start_time, options.interruption);
        }
    };

    if (options.global_state != nullptr) {
        run(*options.global_state);
    } else {
        auto local_state = GlobalState{};
        run(local_state);
    }
}

} // namespace bg
