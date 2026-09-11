#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/node.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
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
class ItemSubscribers;
class SimulationEvaluationEngine;
class NestedGraph;

class RuntimeGraph {
  public:
    explicit RuntimeGraph(const CompiledGraph &compiled_graph);
    RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory);
    RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state);
    RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory, GlobalState &global_state);
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
    std::pmr::memory_resource &memory_resource() const noexcept { return *memory_; }
    void *storage_at(std::size_t offset) noexcept { return storage_ + offset; }
    Node node(std::size_t index) noexcept { return {compiled_graph_->node(index), storage_}; }
    Node node(const CompiledNode &compiled_node) noexcept { return {compiled_node, storage_}; }

  private:
    friend EvaluationContext;
    friend OutputChangeContext;
    friend SimulationEvaluationEngine;
    friend NestedGraph;
    friend details::ReferenceProvider;
    friend testing::RuntimeGraphAccess;

    using ScheduleWord = CompiledGraph::ScheduleWord;
    using SchedulingInfo = CompiledGraph::SchedulingInfo;

    class ScheduledNodes;
    class FutureSchedule;
    class CycleCallbacks;
    class ScratchUse;
    class ModifiedItemPropagator;
    struct PendingInputNotifications;

    auto scheduling_info() noexcept -> std::span<SchedulingInfo>;
    void initialize_storage();

    void schedule(std::size_t node_index) noexcept;
    void schedule_validity_check(std::size_t node_index) noexcept;
    ScheduleHandle schedule_at(std::size_t node_index, EvaluationTime time);
    void cancel(std::size_t node_index, ScheduleHandle handle) noexcept;
    FutureSchedule *&future_schedule() noexcept;
    FutureSchedule *future_schedule() const noexcept;
    void activate_scheduled(EvaluationTime evaluation_time) noexcept;
    void clear_future_schedule() noexcept;
    void mirror_future_schedule();
    CycleCallbacks *&cycle_callbacks() noexcept;
    CycleCallbacks *cycle_callbacks() const noexcept;
    RuntimeGraph &outer_graph() noexcept;
    CycleCallbacks &require_cycle_callbacks();
    void add_cycle_callback(void *receiver, void (*function)(void *) noexcept);
    void remove_cycle_callback(const void *receiver) noexcept;
    void run_cycle_callbacks() noexcept;
    void clear_cycle_callbacks() noexcept;
    void propagate_modified_items(const CompiledNode &node);
    void schedule_subscribers(std::span<const Subscriber> subscribers, OutputChange change);
    void schedule_node_subscriber(std::size_t node_index, bool active, bool required, bool active_optional,
                                  OutputChange change) noexcept;
    void notify_output(const OutputNotification *notification, const TimeSeriesRuntimeType *runtime_type,
                       const void *value, OutputChange change);
    void propagate_output_change(const CompiledNode &node, OutputChange change);
    void publish_output_changes(const CompiledNode &node, bool output_was_valid);
    void schedule_dynamic_subscribers(std::size_t offset, OutputChange change);
    void schedule_dynamic_subscribers(details::DynamicSubscribers &subscribers, OutputChange change);
    void queue_input_notification(InputChangeFunction function, std::size_t node_index, std::size_t data,
                                  OutputChange change);
    void run_input_notifications();
    void apply_invalidation(const CompiledNode &node, std::size_t node_index);
    void stop_nodes(std::size_t node_count) noexcept;
    bool inputs_are_valid(const CompiledNode &node) const noexcept;
    void start(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    void evaluate(EvaluationTime evaluation_time, details::EvaluationScratch &scratch);
    void stop(details::EvaluationScratch &scratch) noexcept;

    const CompiledGraph *compiled_graph_;
    std::pmr::memory_resource *memory_;
    std::unique_ptr<GlobalState> owned_global_state_;
    GlobalState *global_state_;
    std::byte *storage_{};
    EvaluationTime last_evaluation_time_{MIN_DT};
    EvaluationTime evaluation_time_{MIN_DT};
    details::EvaluationScratch *scratch_{};
    RuntimeGraph *parent_{};
    std::size_t owner_node_index_{};
    ScheduleHandle parent_schedule_{};
    std::optional<EvaluationTime> parent_schedule_time_;
    PendingInputNotifications *pending_input_notifications_{};
    bool suppress_parent_scheduling_ : 1 {};
    bool started_ : 1 {};
    bool stopped_ : 1 {};
};

} // namespace bg
