#pragma once

#include <bg/evaluation_time.hpp>

namespace bg {

class RuntimeGraph;
class EvaluationProfile;

class SimulationEvaluationEngine {
  public:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time) const;
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile &profile) const;

  private:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile *profile) const;
};

} // namespace bg
