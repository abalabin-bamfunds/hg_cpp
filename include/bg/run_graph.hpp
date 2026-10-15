#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/simulation_evaluation_engine.hpp>

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
    const auto evaluation_time = MIN_DT + EvaluationTime::duration{1};

    auto run = [&](GlobalState &global_state) {
        auto runtime = RuntimeGraph{compiled, global_state};
        if (options.recorder) {
            options.recorder->run(runtime, evaluation_time);
        } else {
            SimulationEvaluationEngine{}.run(runtime, evaluation_time);
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
