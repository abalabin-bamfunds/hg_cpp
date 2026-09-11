#include <bg/simulation_evaluation_engine.hpp>

#include <bg/details/evaluation_scratch.hpp>
#include <bg/graph/runtime_graph.hpp>

namespace bg {

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time) const {
    auto scratch = details::EvaluationScratch{runtime_graph.allocator_.memory_resource()};
    runtime_graph.start(start_time, scratch);
    runtime_graph.evaluate(start_time, scratch);

    while (const auto time = runtime_graph.next_scheduled_time()) {
        runtime_graph.evaluate(*time, scratch);
    }

    runtime_graph.stop(scratch);
}

} // namespace bg
