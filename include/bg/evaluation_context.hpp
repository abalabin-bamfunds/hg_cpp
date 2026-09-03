#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>
#include <bg/scratch_pad.hpp>

#include <cstddef>

namespace bg {

class RuntimeGraph;
class GlobalState;
class EvaluationContext {
  public:
    ~EvaluationContext();

    [[nodiscard]] const EvaluationTime &evaluation_time() const noexcept { return evaluation_time_; }

    [[nodiscard]] RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }

    [[nodiscard]] GlobalState &global_state() const noexcept;

    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_; }

    [[nodiscard]] ScratchPad &scratch() noexcept { return scratch_; }

    void schedule_now() const noexcept;
    ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const noexcept;

  private:
    friend RuntimeGraph;

    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept;

    EvaluationTime evaluation_time_;
    RuntimeGraph *runtime_graph_;
    std::size_t node_index_;
    ScratchPad scratch_;
};

} // namespace bg
