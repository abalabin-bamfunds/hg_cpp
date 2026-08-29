#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>

#include <cstddef>

namespace bg {

namespace details {
class EvaluationScratch;
}

class RuntimeGraph;
class GlobalState;
class TimeSeriesStorageRepresentation;

class EvaluationContext {
  public:
    ~EvaluationContext();

    [[nodiscard]] const EvaluationTime &evaluation_time() const noexcept { return evaluation_time_; }

    [[nodiscard]] RuntimeGraph &runtime_graph() const noexcept { return *runtime_graph_; }

    [[nodiscard]] GlobalState &global_state() const noexcept;

    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_; }

    void schedule_now() const noexcept;
    [[nodiscard]] ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const noexcept;

  private:
    friend RuntimeGraph;
    friend class TimeSeriesOutputView;

    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept;

    [[nodiscard]] void *add_output_mutation(const TimeSeriesStorageRepresentation &representation,
                                            EvaluationTime &last_updated, void *value, std::size_t item, void *parent);
    [[nodiscard]] static EvaluationContext &output_mutation_context(void *mutation) noexcept;
    void publish_output_mutation(void *mutation) const noexcept;
    void schedule_output_mutations() const noexcept;

    EvaluationTime evaluation_time_;
    RuntimeGraph *runtime_graph_;
    std::size_t node_index_;
    void *scratch_frame_{};
};

} // namespace bg
