#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/evaluation_profile.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace bg {

class RuntimeGraph;

struct EvaluationProfilerStats {
    std::uint64_t evaluations{};
    std::uint64_t checkpoint_ticks{};
};

struct EvaluationProfilerCycleStats {
    std::uint64_t cycles{};
    std::uint64_t total_nanoseconds{};
    std::uint64_t min_nanoseconds{};
    std::uint64_t max_nanoseconds{};
};

struct EvaluationProfilerCyclePeriod {
    EvaluationTime start;
    EvaluationProfilerCycleStats timing;
};

struct EvaluationProfilerNodePeriod {
    std::uint32_t node_index;
    EvaluationProfilerStats totals;
};

struct EvaluationProfilerConfig {
    std::chrono::microseconds node_period{std::chrono::minutes{1}};
    std::size_t retained_node_periods{30};
    std::chrono::microseconds outer_period{std::chrono::seconds{1}};
    std::size_t retained_outer_periods{24 * 60 * 60};
};

struct EvaluationProfilerGraph {
    GraphInstanceId graph_id{};
    GraphInstanceId parent_graph_id{};
    std::uint32_t owner_node_index{};
    const CompiledGraph *compiled_graph{}; // Available only while the graph is active.
    std::size_t node_count{};
    std::optional<ScalarValue<>> label;
    std::optional<EvaluationTime> last_updated;
    EvaluationProfilerStats totals;
    bool active{};
};

// Consumes committed profile records on one thread. Reused graph IDs aggregate
// successive starts into the same period results.
class EvaluationProfiler {
  public:
    class Recorder {
      public:
        explicit Recorder(EvaluationProfiler &profiler) : profiler_{profiler} {}
        void run(RuntimeGraph &runtime_graph, EvaluationTime start_time,
                 EvaluationEngine engine = EvaluationEngine::simulation, std::stop_token stop_token = {}) const;

      private:
        EvaluationProfiler &profiler_;
    };

    explicit EvaluationProfiler(EvaluationProfilerConfig config = {}, allocator memory = {});
    ~EvaluationProfiler();

    EvaluationProfiler(const EvaluationProfiler &) = delete;
    EvaluationProfiler &operator=(const EvaluationProfiler &) = delete;

    Recorder recorder() noexcept { return Recorder{*this}; }
    std::optional<GraphInstanceId> root_graph_id() const noexcept;

    std::size_t poll(EvaluationProfileReader &reader);
    void consume(EvaluationProfileRecord record);
    // The caller guarantees that no later record can contain a cycle started before this time.
    void advance_to(EvaluationTime wall_clock_time);

    const EvaluationProfilerGraph *graph(GraphInstanceId graph_id) const;
    std::vector<GraphInstanceId> graph_ids() const;
    std::optional<EvaluationProfilerStats> node_totals(GraphInstanceId graph_id, std::uint32_t node_index) const;

    // Closed periods only. A period contains complete outer cycles that started in it.
    std::optional<EvaluationProfilerStats> graph_period(GraphInstanceId graph_id,
                                                        EvaluationTime period_start) const;
    std::optional<EvaluationProfilerStats> node_period(GraphInstanceId graph_id, std::uint32_t node_index,
                                                       EvaluationTime period_start) const;
    std::vector<EvaluationTime> node_period_starts() const;
    std::vector<EvaluationProfilerNodePeriod> node_periods(GraphInstanceId graph_id,
                                                            EvaluationTime period_start) const;

    EvaluationProfilerCycleStats outer_totals() const;
    std::optional<EvaluationProfilerCycleStats> outer_period(EvaluationTime period_start) const;
    std::vector<EvaluationProfilerCyclePeriod> outer_periods() const;
    std::string report(std::size_t recent_outer_periods = 10) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bg
