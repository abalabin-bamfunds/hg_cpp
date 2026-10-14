#pragma once

#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_scheduling.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstddef>
#include <optional>

namespace bg {

class EvaluationContext;

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

    void start(RuntimeGraph &graph, EvaluationContext &context, const RuntimeGraphInput *inputs,
               std::optional<Scalar<>> label);
    static bool evaluate(RuntimeGraph &graph, EvaluationContext &context, const RuntimeGraphInput *inputs);
    void stop(RuntimeGraph &graph, EvaluationContext &context, const RuntimeGraphInput *inputs) noexcept;

  private:
    friend RuntimeGraph;

    void require_owner(RuntimeGraph &graph, std::size_t node_index, void *receiver,
                       const RuntimeGraphSchedulingCallbacks *callbacks);
    template <bool Profiled>
    static bool evaluate_graph(RuntimeGraph &graph, EvaluationContext &context, EvaluationProfile *profile,
                               GraphInstanceId graph_id, const RuntimeGraphInput *inputs);
    void notify_scheduled(RuntimeGraph &graph) noexcept;
    void notify_timed_schedule_changed(RuntimeGraph &graph, std::optional<EvaluationTime> previous);
    RuntimeGraph &outer_graph(RuntimeGraph &graph) const noexcept;

    struct DirectOwner {
        ScheduleHandle timed_schedule;
    };

    struct CallbackOwner {
        void *receiver;
        const RuntimeGraphSchedulingCallbacks *callbacks;
    };

    union Owner {
        constexpr Owner() noexcept : direct{} {}

        DirectOwner direct;
        CallbackOwner callback;
    } owner_;

    // The tag selects the callback owner; an untagged parent uses direct scheduling.
    util::tagged_ptr<RuntimeGraph, 1, bool> parent_;
    std::size_t owner_node_index_{};
    EvaluationProfile *profile_{};
};

static_assert(sizeof(RuntimeGraphEvaluator) == 5 * sizeof(void *));

} // namespace bg
