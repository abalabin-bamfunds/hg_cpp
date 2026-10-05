#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>

#include <cstddef>
#include <optional>

namespace bg {

class RuntimeGraph;

// Gives the recipient of inner-graph scheduling notifications access to the parent node's scheduling operations.
class RuntimeGraphSchedulingContext {
  public:
    RuntimeGraphSchedulingContext(RuntimeGraph &graph, std::size_t node_index) noexcept
        : graph_{&graph}, node_index_{node_index} {}

    ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const noexcept;

  private:
    RuntimeGraph *graph_;
    std::size_t node_index_;
};

struct RuntimeGraphSchedulingCallbacks {
    using token_type = std::size_t;

    // Records that this inner graph has immediate work. The runtime schedules the owning node after this callback.
    void (*inner_scheduled)(void *receiver, token_type token) noexcept;

    // Updates the scheduled time for one owned inner graph and mirrors the owner's earliest time through context.
    void (*timed_schedule_changed)(RuntimeGraphSchedulingContext &context, void *receiver, token_type token,
                                    std::optional<EvaluationTime> time);
};

struct RuntimeGraphSchedulingHandler {
    // The receiver and callback record remain valid until every graph using this evaluator stops.
    void *receiver;
    const RuntimeGraphSchedulingCallbacks *callbacks;
};

} // namespace bg
