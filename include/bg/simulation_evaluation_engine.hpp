#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/evaluation_interruption.hpp>

namespace bg {

class RuntimeGraph;
class EvaluationProfile;

class SimulationEvaluationEngine {
  public:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
             const EvaluationInterruption &interruption = {}) const;
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile &profile,
             const EvaluationInterruption &interruption = {}) const;

  private:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile *profile,
             const EvaluationInterruption &interruption) const;
};

} // namespace bg
