#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/periodic_callback.hpp>

#include <optional>

namespace bg {

class RuntimeGraph;
class EvaluationProfile;

class SimulationEvaluationEngine {
  public:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
             const PeriodicCallback &periodic_callback = {},
             std::optional<EvaluationTime> stop_time = std::nullopt) const;
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile &profile,
             const PeriodicCallback &periodic_callback = {},
             std::optional<EvaluationTime> stop_time = std::nullopt) const;

  private:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile *profile,
             const PeriodicCallback &periodic_callback, std::optional<EvaluationTime> stop_time) const;
};

} // namespace bg
