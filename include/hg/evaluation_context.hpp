#pragma once

#include <bg/details/evaluation_context.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/extension/invocation_end_callback.hpp>
#include <bg/graph/runtime_graph_input.hpp>
#include <bg/schedule_handle.hpp>
#include <bg/scratch_pad.hpp>
#include <bg/wall_clock.hpp>

#include <cstddef>

namespace bg {

class RuntimeGraph;
class RuntimeGraphEvaluator;
class GlobalState;
class EvaluationProfile;
namespace details {
class EvaluationScratch;
class InvocationEndCallbackScope;
class NodeScheduler;
} // namespace details

// Engine state borrowed by every EvaluationContext in one run. The engine keeps this object, its referenced services,
// and the root graph alive until all contexts for the run are destroyed.
struct GlobalEvaluationContext {
    const WallClock &wall_clock;
    RuntimeGraph &root_runtime_graph;
    RuntimeGraphEvaluator *root_runtime_graph_evaluator;
    GlobalState &global_state;
    details::EvaluationScratch &evaluation_scratch;
    EvaluationProfile *evaluation_profile;
};

class EvaluationContext {
  public:
    ~EvaluationContext();

    EvaluationContext(const EvaluationContext &) = delete;
    EvaluationContext &operator=(const EvaluationContext &) = delete;
    EvaluationContext(EvaluationContext &&) noexcept = default;
    EvaluationContext &operator=(EvaluationContext &&) noexcept = default;

    // Engine API. The global context and its referenced services must outlive this invocation context.
    EvaluationContext(GlobalEvaluationContext &global_context, EvaluationTime evaluation_time,
                      RuntimeGraph &runtime_graph, const RuntimeGraphInput *runtime_graph_inputs = nullptr) noexcept
        : global_context_{&global_context}, evaluation_time_{evaluation_time}, runtime_graph_{&runtime_graph},
          node_index_{}, scratch_{&global_context.evaluation_scratch}, runtime_graph_inputs_{runtime_graph_inputs} {}

    const EvaluationTime &evaluation_time() const noexcept { return evaluation_time_; }

    const WallClock &wall_clock() const noexcept { return global_context_->wall_clock; }

    RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }

    EvaluationProfile *evaluation_profile() const noexcept { return global_context_->evaluation_profile; }

    GlobalState &global_state() const noexcept { return global_context_->global_state; }

    GlobalEvaluationContext &global_context() const noexcept { return *global_context_; }

    std::size_t node_index() const noexcept { return node_index_; }

    ScratchPad &scratch() noexcept { return shared_scratch_ == nullptr ? scratch_ : *shared_scratch_; }

    void schedule_now() const noexcept;
    ScheduleHandle schedule_at(EvaluationTime time) const;
    // Engine API for nodes that schedule a different node in the current graph.
    ScheduleHandle schedule_at(std::size_t node_index, EvaluationTime time) const;
    ScheduleHandle schedule_wall_clock_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const noexcept;
    bool timer_fired(ScheduleHandle handle) const noexcept;
    void add_cycle_callback(void *receiver, void (*function)(void *, EvaluationContext &) noexcept,
                           bool call_on_stop = false) const;
    void add_invocation_end_callback(extension::InvocationEndRegistration &registration, void *receiver,
                                     void (*function)(void *, EvaluationContext &)) const;

    EvaluationContext child(details::EvaluationContextSection &section) noexcept;
    EvaluationContext child(RuntimeGraph &runtime_graph, const RuntimeGraphInput *inputs) noexcept;

    const details::EvaluationContextSection *find_section(const void *type,
                                                          std::size_t depth = 0) const noexcept;

    const RuntimeGraphInput *runtime_graph_inputs() const noexcept { return runtime_graph_inputs_; }

  private:
    friend RuntimeGraph;
    friend RuntimeGraphEvaluator;
    friend details::InvocationEndCallbackScope;
    friend details::NodeScheduler;
    EvaluationContext(EvaluationContext &parent, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept
        : global_context_{parent.global_context_}, evaluation_time_{parent.evaluation_time_},
          runtime_graph_{&runtime_graph}, node_index_{node_index},
          scratch_{&parent.global_context_->evaluation_scratch}, sections_{parent.sections_},
          runtime_graph_inputs_{parent.runtime_graph_inputs_} {}
    EvaluationContext(RuntimeGraph &runtime_graph, EvaluationContext &parent,
                      const RuntimeGraphInput *runtime_graph_inputs) noexcept
        : global_context_{parent.global_context_}, evaluation_time_{parent.evaluation_time_},
          runtime_graph_{&runtime_graph}, node_index_{}, scratch_{&parent.global_context_->evaluation_scratch},
          sections_(parent.sections_), runtime_graph_inputs_{runtime_graph_inputs} {}
    EvaluationContext(EvaluationContext &parent, const details::EvaluationContextSection &section) noexcept
        : global_context_{parent.global_context_}, evaluation_time_{parent.evaluation_time_},
          runtime_graph_{parent.runtime_graph_}, node_index_{parent.node_index_},
          scratch_{&parent.global_context_->evaluation_scratch}, shared_scratch_{&parent.scratch()},
          invocation_end_callbacks_{parent.invocation_end_callbacks_}, sections_(&section),
          runtime_graph_inputs_{parent.runtime_graph_inputs_} {}

    void reset_scratch() noexcept { scratch_.reset(); }

    GlobalEvaluationContext *global_context_;
    EvaluationTime evaluation_time_;
    RuntimeGraph *runtime_graph_;
    std::size_t node_index_;
    ScratchPad scratch_;
    ScratchPad *shared_scratch_{};
    details::InvocationEndCallbackScope *invocation_end_callbacks_{};
    const details::EvaluationContextSection *sections_{};
    const RuntimeGraphInput *runtime_graph_inputs_{};
};

} // namespace bg
