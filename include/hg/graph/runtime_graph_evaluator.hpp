#pragma once

#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_scheduling.hpp>
#include <bg/schedule_handle.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstddef>
#include <optional>

namespace bg {

class EvaluationContext;
struct GlobalEvaluationContext;
class TimerScheduler;

// Runs runtime graphs owned by the same node.
class RuntimeGraphEvaluator {
  public:
    static constexpr bool supports_byte_copy_initialization = true;

    RuntimeGraphEvaluator() noexcept = default;

    RuntimeGraphEvaluator(const RuntimeGraphEvaluator &) = delete;
    RuntimeGraphEvaluator &operator=(const RuntimeGraphEvaluator &) = delete;
    RuntimeGraphEvaluator(RuntimeGraphEvaluator &&) = delete;
    RuntimeGraphEvaluator &operator=(RuntimeGraphEvaluator &&) = delete;

    void bind_owner(RuntimeGraph &graph, std::size_t node_index);
    void bind_owner(RuntimeGraph &graph, std::size_t node_index, RuntimeGraphSchedulingHandler handler);
    bool owner_bound() const noexcept { return parent_.get() != nullptr; }

    // These timer operations are called only on the root evaluator. Child evaluators use owner_ for notifications.
    ScheduleHandle schedule_at(GlobalEvaluationContext &context, RuntimeGraph &graph, std::size_t node_index,
                               EvaluationTime time);
    ScheduleHandle schedule_wall_clock_at(GlobalEvaluationContext &context, RuntimeGraph &graph,
                                          std::size_t node_index, EvaluationTime time);
    void cancel(RuntimeGraph &graph, std::size_t node_index, ScheduleHandle handle) noexcept;
    bool timer_fired(RuntimeGraph &graph, std::size_t node_index, ScheduleHandle handle) const noexcept;
    std::optional<EvaluationTime> next_scheduled_time() const noexcept;
    std::optional<EvaluationTime> next_wall_clock_time() const noexcept;
    void retire_fired() noexcept;
    bool activate_due(GlobalEvaluationContext &context, EvaluationTime evaluation_time) noexcept;
    void clear_timers(RuntimeGraph &graph) noexcept;
    void destroy_timer_scheduler(RuntimeGraph &root_graph) noexcept;

    void start(RuntimeGraph &graph, EvaluationContext &context, const RuntimeGraphInput *inputs,
               std::optional<Scalar<>> label);
    static bool evaluate(RuntimeGraph &graph, EvaluationContext &context, const RuntimeGraphInput *inputs);
    void stop(RuntimeGraph &graph, EvaluationContext &context, const RuntimeGraphInput *inputs) noexcept;

  private:
    friend RuntimeGraph;

    void require_owner(RuntimeGraph &graph, std::size_t node_index, RuntimeGraphSchedulingHandler handler);
    template <bool Profiled>
    static bool evaluate_graph(RuntimeGraph &graph, EvaluationContext &context, EvaluationProfile *profile,
                               GraphInstanceId graph_id, const RuntimeGraphInput *inputs);
    void notify_scheduled(RuntimeGraph &graph) noexcept;
    RuntimeGraph &outer_graph(RuntimeGraph &graph) const noexcept;

    union Owner {
        constexpr Owner() noexcept : scheduler{} {}

        TimerScheduler *scheduler;
        RuntimeGraphSchedulingHandler callback;
    } owner_;

    // The tag selects the owner scheduling callback.
    util::tagged_ptr<RuntimeGraph, 1, bool> parent_;
    std::size_t owner_node_index_{};
    EvaluationProfile *profile_{};
};

static_assert(sizeof(RuntimeGraphEvaluator) == 5 * sizeof(void *));

} // namespace bg
