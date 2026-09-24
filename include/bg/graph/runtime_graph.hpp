#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph_owner.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bg {

namespace details {
class EvaluationScratch;
class ReferenceProvider;
} // namespace details

namespace testing {
class RuntimeGraphAccess;
}

class TimeSeriesRuntimeType;
class ItemSubscription;
class SimulationEvaluationEngine;
class NestedGraph;

class RuntimeGraph {
  public:
    explicit RuntimeGraph(const CompiledGraph &compiled_graph);
    RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory);
    RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state);
    RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, GlobalState &global_state);
    ~RuntimeGraph();

    RuntimeGraph(const RuntimeGraph &) = delete;
    RuntimeGraph &operator=(const RuntimeGraph &) = delete;
    RuntimeGraph(RuntimeGraph &&) = delete;
    RuntimeGraph &operator=(RuntimeGraph &&) = delete;

    void start(EvaluationTime evaluation_time);
    void stop() noexcept;
    void evaluate(EvaluationTime evaluation_time);
    std::optional<EvaluationTime> next_scheduled_time() const noexcept;
    EvaluationTime evaluation_time() const noexcept { return evaluation_time_; }
    GlobalState &global_state() const noexcept { return *global_state_; }
    bg::allocator &allocation() noexcept { return allocator_; }
    const bg::allocator &allocation() const noexcept { return allocator_; }
    void *storage_at(std::size_t offset) noexcept { return storage_ + offset; }
    Node node(std::size_t index) noexcept { return {compiled_graph_->node(index), storage_}; }
    Node node(const CompiledNode &compiled_node) noexcept { return {compiled_node, storage_}; }

  private:
    friend EvaluationContext;
    friend OutputChangeContext;
    friend SimulationEvaluationEngine;
    friend NestedGraph;
    friend RuntimeGraphOwnerContext;
    friend details::ReferenceProvider;
    friend testing::RuntimeGraphAccess;

    using ScheduleWord = CompiledGraph::ScheduleWord;
    using SchedulingInfo = CompiledGraph::SchedulingInfo;

    class ScheduledNodes;
    class TimedSchedule;
    class CycleCallbacks;
    class ScratchUse;
    class OutputChangePublisher;
    struct PendingInputNotifications;

    auto scheduling_info() noexcept -> std::span<SchedulingInfo>;
    void initialize_storage();

    void schedule(std::size_t node_index) noexcept;
    void schedule_validity_check(std::size_t node_index) noexcept;
    ScheduleHandle schedule_at(std::size_t node_index, EvaluationTime time);
    void cancel(std::size_t node_index, ScheduleHandle handle) noexcept;
    TimedSchedule *&timed_schedule() noexcept;
    TimedSchedule *timed_schedule() const noexcept;
    void activate_scheduled(EvaluationTime evaluation_time) noexcept;
    void clear_timed_schedule() noexcept;
    void notify_timed_schedule_changed(std::optional<EvaluationTime> previous);
    void notify_owner_scheduled() noexcept;
    void set_owner(RuntimeGraph &graph, std::size_t node_index) noexcept;
    void set_owner(RuntimeGraph &graph, std::size_t node_index, RuntimeGraphOwner owner) noexcept;
    void clear_owner() noexcept;
    CycleCallbacks *&cycle_callbacks() noexcept;
    CycleCallbacks *cycle_callbacks() const noexcept;
    RuntimeGraph &outer_graph() noexcept;
    CycleCallbacks &require_cycle_callbacks();
    void add_cycle_callback(void *receiver, void (*function)(void *) noexcept);
    void remove_cycle_callback(const void *receiver) noexcept;
    void run_cycle_callbacks() noexcept;
    void clear_cycle_callbacks() noexcept;
    std::optional<OutputChange> output_change(const CompiledNode &node, bool output_was_valid) const noexcept;
    void publish_output(const CompiledNode &node, OutputChange change);
    void notify_subscribers(std::span<const Subscriber> subscribers, OutputChange change);
    void notify_node_subscriber(std::size_t node_index, bool active, bool required, bool active_optional,
                                OutputChange change) noexcept;
    void notify_dynamic_subscribers(details::DynamicSubscribers &subscribers, OutputChange change);
    void queue_input_notification(InputNotificationFunction function, std::size_t node_index, std::size_t data,
                                  OutputChange change);
    void run_input_notifications();
    void apply_invalidation(const CompiledNode &node, std::size_t node_index);
    void stop_nodes(std::size_t node_count) noexcept;
    bool inputs_are_valid(const CompiledNode &node) const noexcept;
    void start(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    void start(EvaluationContext &parent);
    void evaluate(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    bool continue_evaluation(EvaluationContext &parent);
    void stop(details::EvaluationScratch &scratch) noexcept;
    void stop(EvaluationContext &parent) noexcept;

    const CompiledGraph *compiled_graph_;
    bg::allocator allocator_;
    std::unique_ptr<GlobalState> owned_global_state_;
    GlobalState *global_state_;
    std::byte *storage_{};
    EvaluationTime last_evaluation_time_{MIN_DT};
    EvaluationTime evaluation_time_{MIN_DT};
    details::EvaluationScratch *scratch_{};
    struct DirectOwner {
        std::size_t node_index{};
        ScheduleHandle timed_schedule{};
    };

    struct CallbackOwner {
        std::size_t node_index{};
        RuntimeGraphOwner owner{};
    };

    // An inner graph uses either the compact direct nested() connection or callbacks to an owner of several graphs.
    union Owner {
        constexpr Owner() noexcept : direct{} {}

        DirectOwner direct;
        CallbackOwner callback;
    } owner_;

    RuntimeGraph *parent_{};
    PendingInputNotifications *pending_input_notifications_{};
    std::uint32_t paused_node_index_{std::numeric_limits<std::uint32_t>::max()};
    bool suppress_parent_scheduling_ : 1 {};
    bool callback_owner_ : 1 {};
    bool started_ : 1 {};
    bool stopped_ : 1 {};
};

} // namespace bg
