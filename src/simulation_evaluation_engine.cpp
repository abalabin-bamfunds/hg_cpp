#include <bg/simulation_evaluation_engine.hpp>

#include <bg/graph/runtime_graph.hpp>

namespace bg {

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time) const {
    runtime_graph.start(start_time);
    runtime_graph.evaluate(start_time);

    while (const auto time = runtime_graph.next_scheduled_time()) {
        runtime_graph.evaluate(*time);
    }

    runtime_graph.stop();
}

} // namespace bg
