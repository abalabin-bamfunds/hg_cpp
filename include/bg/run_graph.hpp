#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/global_state.hpp>
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
namespace details {

template <std::invocable Function>
void run_graph(Function &&function, GlobalState *global_state, const ServiceCatalogue *service_catalogue) {
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        (void)std::invoke(std::forward<Function>(function));
    }

    if (service_catalogue != nullptr) {
        wiring.finalize(*service_catalogue);
    } else {
        wiring.finalize();
    }

    auto compiled = CompiledGraph{wiring};
    const auto evaluation_time = MIN_DT + EvaluationTime::duration{1};

    if (global_state != nullptr) {
        auto runtime = RuntimeGraph{compiled, *global_state};
        SimulationEvaluationEngine{}.run(runtime, evaluation_time);
        return;
    }

    auto local_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, local_state};
    SimulationEvaluationEngine{}.run(runtime, evaluation_time);
}

} // namespace details

template <std::invocable Function> void run_graph(Function &&function) {
    details::run_graph(std::forward<Function>(function), nullptr, nullptr);
}

template <std::invocable Function> void run_graph(Function &&function, GlobalState &global_state) {
    details::run_graph(std::forward<Function>(function), &global_state, nullptr);
}

template <std::invocable Function> void run_graph(Function &&function, const ServiceCatalogue &service_catalogue) {
    details::run_graph(std::forward<Function>(function), nullptr, &service_catalogue);
}

template <std::invocable Function>
void run_graph(Function &&function, GlobalState &global_state, const ServiceCatalogue &service_catalogue) {
    details::run_graph(std::forward<Function>(function), &global_state, &service_catalogue);
}

} // namespace bg
