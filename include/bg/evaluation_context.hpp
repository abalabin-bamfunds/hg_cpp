#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>

#include <cstddef>

namespace bg {

namespace details {
class EvaluationScratch;
template <typename View> class output_view_proxy;
}

class RuntimeGraph;
class GlobalState;
class TimeSeriesOutputView;
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
    template <typename View> friend class details::output_view_proxy;

    EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph, std::size_t node_index) noexcept;

    [[nodiscard]] void *add_output_path(const TimeSeriesStorageRepresentation &representation,
                                        EvaluationTime &last_updated, void *value, std::size_t item, void *parent);
    [[nodiscard]] TimeSeriesOutputView &add_output_view(const TimeSeriesOutputView &prototype);
    [[nodiscard]] static EvaluationContext &output_path_context(void *path) noexcept;
    void publish_output_path(void *path) const noexcept;
    void release_output_view(void *view) noexcept;
    void release_output_path(void *path) noexcept;
    void finalize_output(EvaluationTime &last_updated, void *value) const noexcept;

    EvaluationTime evaluation_time_;
    RuntimeGraph *runtime_graph_;
    std::size_t node_index_;
    void *scratch_frame_{};
    const TimeSeriesStorageRepresentation *output_representation_{};
};

} // namespace bg
