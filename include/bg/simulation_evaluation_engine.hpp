#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/periodic_callback.hpp>

namespace bg {

class RuntimeGraph;
class EvaluationProfile;

class SimulationEvaluationEngine {
  public:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
             const PeriodicCallback &periodic_callback = {}) const;
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile &profile,
             const PeriodicCallback &periodic_callback = {}) const;

  private:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile *profile,
             const PeriodicCallback &periodic_callback) const;
};

} // namespace bg
