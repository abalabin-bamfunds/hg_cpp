#pragma once

#include <bg/details/evaluation_context.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>
#include <bg/scratch_pad.hpp>
#include <bg/wall_clock.hpp>

#include <cstddef>

namespace bg {

class RuntimeGraph;
class GlobalState;
class SimulationEvaluationEngine;
namespace details {
class EvaluationScratch;
class NodeScheduler;
} // namespace details

class EvaluationContext {
  public:
    ~EvaluationContext();

    EvaluationContext(const EvaluationContext &) = delete;
    EvaluationContext &operator=(const EvaluationContext &) = delete;
    EvaluationContext(EvaluationContext &&) noexcept = default;
    EvaluationContext &operator=(EvaluationContext &&) noexcept = default;

    const EvaluationTime &evaluation_time() const noexcept { return evaluation_time_; }

    const WallClock &wall_clock() const noexcept { return *wall_clock_; }

    RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }

    GlobalState &global_state() const noexcept;

    std::size_t node_index() const noexcept { return node_index_; }

    ScratchPad &scratch() noexcept { return scratch_; }

    void schedule_now() const noexcept;
    ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const noexcept;
    void add_cycle_callback(void *receiver, void (*function)(void *) noexcept) const;

    EvaluationContext child(details::EvaluationContextSection &section) noexcept;

    const details::EvaluationContextSection *find_section(const void *type,
                                                          std::size_t depth = 0) const noexcept;

  private:
    friend RuntimeGraph;
    friend SimulationEvaluationEngine;
    friend details::NodeScheduler;

    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph,
                      details::EvaluationScratch &evaluation_scratch, const WallClock &wall_clock) noexcept
        : evaluation_time_(evaluation_time), wall_clock_(&wall_clock), runtime_graph_(&runtime_graph), node_index_{},
          scratch_{&evaluation_scratch} {}
    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, std::size_t node_index,
                      details::EvaluationScratch *evaluation_scratch, const WallClock &wall_clock,
                      const details::EvaluationContextSection *sections = nullptr) noexcept;
    EvaluationContext(EvaluationContext &parent, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept
        : evaluation_time_(parent.evaluation_time_), wall_clock_(parent.wall_clock_), runtime_graph_(&runtime_graph),
          node_index_(node_index), scratch_{parent.scratch_.evaluation_scratch_}, sections_(parent.sections_) {}
    EvaluationContext(EvaluationContext &parent, const details::EvaluationContextSection &section) noexcept
        : evaluation_time_(parent.evaluation_time_), wall_clock_(parent.wall_clock_),
          runtime_graph_(parent.runtime_graph_), node_index_(parent.node_index_),
          scratch_{parent.scratch_.evaluation_scratch_}, sections_(&section) {}

    EvaluationTime evaluation_time_;
    const WallClock *wall_clock_;
    RuntimeGraph *runtime_graph_;
    std::size_t node_index_;
    ScratchPad scratch_;
    const details::EvaluationContextSection *sections_{};
};

} // namespace bg
