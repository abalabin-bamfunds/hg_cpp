#pragma once

#include <bg/details/evaluation_context.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>
#include <bg/scratch_pad.hpp>

#include <cstddef>

namespace bg {

class RuntimeGraph;
class GlobalState;
namespace details {
class EvaluationScratch;
} // namespace details

class EvaluationContext {
  public:
    ~EvaluationContext();

    const EvaluationTime &evaluation_time() const noexcept { return evaluation_time_; }

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

    void set_pending(details::PendingEvaluation pending) noexcept { pending_ = pending; }

  private:
    friend RuntimeGraph;

    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, std::size_t node_index,
                      const details::EvaluationContextSection *sections = nullptr) noexcept;
    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph,
                      details::EvaluationScratch &evaluation_scratch) noexcept;
    EvaluationContext(EvaluationContext &parent, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept;
    EvaluationContext(EvaluationContext &parent, const details::EvaluationContextSection &section) noexcept;

    EvaluationTime evaluation_time_;
    RuntimeGraph *runtime_graph_;
    details::EvaluationScratch *evaluation_scratch_;
    std::size_t node_index_;
    ScratchPad scratch_;
    const details::EvaluationContextSection *sections_{};
    details::PendingEvaluation pending_;
};

} // namespace bg
