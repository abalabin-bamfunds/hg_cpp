#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/periodic_callback.hpp>

#include <stop_token>

namespace bg {

class RuntimeGraph;
class EvaluationProfile;

// Runs a fresh outer runtime graph from start_time. After its initial cycle, run waits while the graph is idle and
// returns when stop_token requests stop. The caller must not start, evaluate, or stop the graph independently.
class RealtimeEvaluationEngine {
  public:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, std::stop_token stop_token = {},
             const PeriodicCallback &periodic_callback = {}) const;
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile &profile,
             std::stop_token stop_token = {}, const PeriodicCallback &periodic_callback = {}) const;

  private:
    void run(RuntimeGraph &runtime_graph, EvaluationTime start_time, EvaluationProfile *profile,
             std::stop_token stop_token, const PeriodicCallback &periodic_callback) const;
};

} // namespace bg
