#include <bg/simulation_evaluation_engine.hpp>

#include <bg/details/evaluation_scratch.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>

#include <stdexcept>

namespace bg {

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time) const {
    auto scratch = details::EvaluationScratch{runtime_graph.allocator_.memory_resource()};
    auto start_context = EvaluationContext{start_time, runtime_graph, scratch};
    runtime_graph.start(start_context);

    const auto evaluate = [&](EvaluationTime time) {
        auto context = EvaluationContext{time, runtime_graph, scratch};
        if (!runtime_graph.evaluate(context)) {
            throw std::logic_error{"a pending evaluation escaped the outer graph"};
        }
    };
    evaluate(start_time);

    while (const auto time = runtime_graph.next_scheduled_time()) {
        evaluate(*time);
    }

    auto stop_context = EvaluationContext{runtime_graph.evaluation_time(), runtime_graph, scratch};
    runtime_graph.stop(stop_context);
}

} // namespace bg
