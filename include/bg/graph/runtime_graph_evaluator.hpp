#pragma once

#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_owner.hpp>
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

    void set_owner(RuntimeGraph &graph, std::size_t node_index);
    void set_owner(RuntimeGraph &graph, std::size_t node_index, const RuntimeGraphOwner &owner);

    void start(RuntimeGraph &graph, EvaluationContext &context);
    bool evaluate(RuntimeGraph &graph, EvaluationContext &context);
    void stop(RuntimeGraph &graph, EvaluationContext &context) noexcept;

  private:
    friend RuntimeGraph;

    void require_owner(RuntimeGraph &graph, std::size_t node_index, void *receiver,
                       const RuntimeGraphOwnerCallbacks *callbacks);
    void notify_scheduled(RuntimeGraph &graph) noexcept;
    void notify_timed_schedule_changed(RuntimeGraph &graph, std::optional<EvaluationTime> previous);
    RuntimeGraph &outer_graph(RuntimeGraph &graph) const noexcept;

    struct DirectOwner {
        ScheduleHandle timed_schedule;
    };

    struct CallbackOwner {
        void *receiver;
        const RuntimeGraphOwnerCallbacks *callbacks;
    };

    union Owner {
        constexpr Owner() noexcept : direct{} {}

        DirectOwner direct;
        CallbackOwner callback;
    } owner_;

    // The tag selects the callback owner; an untagged parent uses direct scheduling.
    util::tagged_ptr<RuntimeGraph, 1, bool> parent_;
    std::size_t owner_node_index_{};
};

static_assert(sizeof(RuntimeGraphEvaluator) == 4 * sizeof(void *));

} // namespace bg
