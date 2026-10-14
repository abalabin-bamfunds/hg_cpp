#pragma once

#include <bg/details/evaluation_context.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/graph/runtime_graph_input.hpp>
#include <bg/schedule_handle.hpp>
#include <bg/scratch_pad.hpp>
#include <bg/wall_clock.hpp>

#include <cstddef>

namespace bg {

class RuntimeGraph;
class RuntimeGraphEvaluator;
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
    void add_cycle_callback(void *receiver, void (*function)(void *, EvaluationContext &) noexcept,
                           bool call_on_stop = false) const;

    EvaluationContext child(details::EvaluationContextSection &section) noexcept;
    EvaluationContext child(RuntimeGraph &runtime_graph, const RuntimeGraphInput *inputs) noexcept;

    const details::EvaluationContextSection *find_section(const void *type,
                                                          std::size_t depth = 0) const noexcept;

    const RuntimeGraphInput *runtime_graph_inputs() const noexcept { return runtime_graph_inputs_; }

  private:
    friend RuntimeGraph;
    friend RuntimeGraphEvaluator;
    friend SimulationEvaluationEngine;
    friend details::NodeScheduler;

    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, GlobalState &global_state,
                      details::EvaluationScratch &evaluation_scratch, const WallClock &wall_clock) noexcept
        : evaluation_time_(evaluation_time), wall_clock_(&wall_clock), runtime_graph_(&runtime_graph), node_index_{},
          global_state_{&global_state}, scratch_{&evaluation_scratch} {}
    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, std::size_t node_index,
                      GlobalState &global_state, details::EvaluationScratch *evaluation_scratch,
                      const WallClock &wall_clock,
                      const details::EvaluationContextSection *sections = nullptr) noexcept;
    EvaluationContext(EvaluationContext &parent, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept
        : evaluation_time_(parent.evaluation_time_), wall_clock_(parent.wall_clock_), runtime_graph_(&runtime_graph),
          node_index_(node_index), global_state_(parent.global_state_),
          scratch_{parent.scratch_.evaluation_scratch_}, sections_(parent.sections_),
          runtime_graph_inputs_{parent.runtime_graph_ == &runtime_graph ? parent.runtime_graph_inputs_ : nullptr} {}
    EvaluationContext(EvaluationContext &parent, const details::EvaluationContextSection &section) noexcept
        : evaluation_time_(parent.evaluation_time_), wall_clock_(parent.wall_clock_),
          runtime_graph_(parent.runtime_graph_), node_index_(parent.node_index_),
          global_state_(parent.global_state_), scratch_{parent.scratch_.evaluation_scratch_}, sections_(&section),
          runtime_graph_inputs_{parent.runtime_graph_inputs_} {}

    void reset_scratch() noexcept { scratch_.reset(); }

    EvaluationTime evaluation_time_;
    const WallClock *wall_clock_;
    RuntimeGraph *runtime_graph_;
    std::size_t node_index_;
    GlobalState *global_state_;
    ScratchPad scratch_;
    const details::EvaluationContextSection *sections_{};
    const RuntimeGraphInput *runtime_graph_inputs_{};
};

} // namespace bg
