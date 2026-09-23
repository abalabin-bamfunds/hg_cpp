#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/schedule_handle.hpp>

#include <cstddef>
#include <optional>

namespace bg {

class RuntimeGraph;

// Gives a nested graph owner access to the owning node's scheduling operations.
class RuntimeGraphOwnerContext {
  public:
    ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const noexcept;

  private:
    friend RuntimeGraph;

    RuntimeGraphOwnerContext(RuntimeGraph &graph, std::size_t node_index) noexcept
        : graph_{&graph}, node_index_{node_index} {}

    RuntimeGraph *graph_;
    std::size_t node_index_;
};

struct RuntimeGraphOwnerCallbacks {
    using token_type = std::size_t;

    // Records that this inner graph has immediate work. The runtime schedules the owning node after this callback.
    void (*inner_scheduled)(void *receiver, token_type token) noexcept;

    // Updates the scheduled time for one owned inner graph and mirrors the owner's earliest time through context.
    void (*timed_schedule_changed)(RuntimeGraphOwnerContext &context, void *receiver, token_type token,
                                    std::optional<EvaluationTime> time);
};

struct RuntimeGraphOwner {
    // The receiver and shared callback record remain valid until the inner graph stops.
    void *receiver;
    RuntimeGraphOwnerCallbacks::token_type token;
    const RuntimeGraphOwnerCallbacks *callbacks;
};

} // namespace bg
