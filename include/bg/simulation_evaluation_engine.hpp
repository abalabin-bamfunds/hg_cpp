#pragma once

#include <bg/evaluation_time.hpp>

namespace bg {

class RuntimeGraph;

class SimulationEvaluationEngine {
  public:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time) const;
};

} // namespace bg
