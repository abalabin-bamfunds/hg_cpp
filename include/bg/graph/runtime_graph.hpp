#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/dynamic_node_subscriber.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph_scheduling.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace bg {

namespace details {
struct DynamicSubscribers;
class EvaluationScratch;
class ServiceRequestBinding;
} // namespace details

class NestedGraph;
class RuntimeGraphEvaluator;

class RuntimeGraph {
  public:
    RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state);
    RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, GlobalState &global_state);
    RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, RuntimeGraphEvaluator &evaluator,
                 RuntimeGraphSchedulingCallbacks::token_type owner_token);
    ~RuntimeGraph();

    RuntimeGraph(const RuntimeGraph &) = delete;
    RuntimeGraph &operator=(const RuntimeGraph &) = delete;
    RuntimeGraph(RuntimeGraph &&) = delete;
    RuntimeGraph &operator=(RuntimeGraph &&) = delete;

    // Overloads without a GlobalState or EvaluationContext apply only to outer graphs.
    void start(EvaluationTime evaluation_time);
    void start(EvaluationTime evaluation_time, GlobalState &global_state);
    void stop() noexcept;
    void stop(GlobalState &global_state) noexcept;
    void evaluate(EvaluationTime evaluation_time);
    void evaluate(EvaluationTime evaluation_time, GlobalState &global_state);
    std::optional<EvaluationTime> next_scheduled_time() const noexcept;
    EvaluationTime evaluation_time() const noexcept { return evaluation_time_; }
    GlobalState &global_state() const noexcept;
    bg::allocator &allocation() noexcept { return allocator_; }
    const bg::allocator &allocation() const noexcept { return allocator_; }
    void *storage_at(std::size_t offset) noexcept { return storage_ + offset; }
    Node node(std::size_t index) noexcept { return {compiled_graph_->node(index), storage_}; }
    Node node(const CompiledNode &compiled_node) noexcept { return {compiled_node, storage_}; }
    const CompiledNode &compiled_node(std::size_t index) const noexcept { return compiled_graph_->node(index); }

    void start(EvaluationContext &context);
    bool evaluate(EvaluationContext &context);
    void stop(EvaluationContext &context) noexcept;
    void finalize(EvaluationContext &context) noexcept;
    void finalize(GlobalState &global_state) noexcept;

    void schedule(std::size_t node_index) noexcept;
    void schedule_validity_check(std::size_t node_index) noexcept;
    ScheduleHandle schedule_at(std::size_t node_index, EvaluationTime time);
    void cancel(std::size_t node_index, ScheduleHandle handle) noexcept;

    // Suppresses propagation to the owning node and returns the previous state.
    bool set_owner_scheduling_suppressed(bool suppressed) noexcept;
    bool evaluation_pending() const noexcept;

    void update_reference_output(std::size_t node_index, Reference reference, EvaluationTime evaluation_time);
    void initialize_constant_output(std::size_t node_index, Scalar<> value, EvaluationTime evaluation_time);
    // Publish a borrowed sample using the representation selected when wiring, before its consuming cycle.
    void bind_runtime_input(std::size_t index, const TimeSeriesState &source_state, const void *value,
                            EvaluationTime evaluation_time);
    // Publish an invalid sample before the graph evaluates its supplied cycle.
    void clear_runtime_input(std::size_t index, EvaluationTime evaluation_time);

    void add_cycle_callback(void *receiver, void (*function)(void *, EvaluationContext &) noexcept,
                           bool call_on_stop = false);
    void remove_cycle_callback(const void *receiver) noexcept;
    void notify_dynamic_subscribers(details::DynamicSubscribers &subscribers, OutputChange change);
    void notify_dynamic_subscriber(const details::DynamicNodeSubscriber &subscriber, OutputChange change) noexcept {
        auto active = subscriber.active();
        if (subscriber.controlled_activity()) {
            active = dynamic_input_active(subscriber.input_offset);
        }
        notify_node_subscriber(subscriber.node_index(), active, subscriber.required(),
                               active && !subscriber.required(),
                               change);
    }

  private:
    friend RuntimeGraphEvaluator;

    using ScheduleWord = CompiledGraph::ScheduleWord;
    using SchedulingInfo = CompiledGraph::SchedulingInfo;

    class ScheduledNodes;
    class TimedSchedule;
    class CycleCallbacks;
    class OutputChangePublisher;

    SchedulingInfo &scheduling_info(std::size_t word_index) noexcept {
        if (word_index == 0) {
            return inline_scheduling_;
        }

        const auto runtime_storage_size = compiled_graph_->runtime_storage_size();
        const auto spill_offset =
            (runtime_storage_size + alignof(SchedulingInfo) - 1) & ~(alignof(SchedulingInfo) - 1);
        return reinterpret_cast<SchedulingInfo *>(storage_ + spill_offset)[word_index - 1];
    }
    std::size_t runtime_allocation_size() const noexcept;
    std::size_t runtime_allocation_alignment() const noexcept;
    void initialize_storage();
    void notify_runtime_input(const details::RuntimeInputDescriptor &binding, OutputChange change,
                              EvaluationTime evaluation_time);

    TimedSchedule *&timed_schedule() noexcept;
    TimedSchedule *timed_schedule() const noexcept;
    void activate_scheduled(EvaluationTime evaluation_time) noexcept;
    void clear_timed_schedule() noexcept;
    void notify_timed_schedule_changed(std::optional<EvaluationTime> previous);
    void notify_owner_scheduled() noexcept;
    CycleCallbacks *&cycle_callbacks() noexcept;
    CycleCallbacks *cycle_callbacks() const noexcept;
    RuntimeGraph &outer_graph() noexcept;
    CycleCallbacks &require_cycle_callbacks();
    void run_cycle_callbacks(EvaluationContext &context) noexcept;
    void clear_cycle_callbacks(EvaluationContext &context) noexcept;
    std::optional<OutputChange> output_change(const CompiledNode &node, bool output_was_valid) const noexcept;
    void publish_output(const CompiledNode &node, OutputChange change);
    void notify_subscribers(std::span<const Subscriber> subscribers, OutputChange change);
    void notify_node_subscriber(std::size_t node_index, bool active, bool required, bool active_optional,
                                OutputChange change) noexcept;
    bool dynamic_input_active(std::uint32_t input_offset) const noexcept;
    void initialize_inherited_time_series(std::size_t index, Reference reference);
    std::span<details::ServiceRequestBinding *> service_bindings() noexcept;
    void apply_invalidation(const CompiledNode &node, std::size_t node_index, EvaluationContext &context);
    void stop_nodes(std::size_t node_count, EvaluationContext &context) noexcept;
    bool inputs_are_valid(const CompiledNode &node) const noexcept;
    const CompiledGraph *compiled_graph_;
    bg::allocator allocator_;
    RuntimeGraphEvaluator *evaluator_;
    std::byte *storage_{};
    EvaluationTime last_evaluation_time_{MIN_DT};
    EvaluationTime evaluation_time_{MIN_DT};
    // Only outer graphs use the global state pointer; inner graphs use the owner token.
    union {
        GlobalState *root_global_state_;
        RuntimeGraphSchedulingCallbacks::token_type owner_token_;
    };
    std::uint32_t paused_node_index_{std::numeric_limits<std::uint32_t>::max()};
    bool suppress_parent_scheduling_ : 1 {};
    bool owns_evaluator_ : 1 {};
    bool started_ : 1 {};
    bool stopped_ : 1 {};
    bool finalized_ : 1 {};
    SchedulingInfo inline_scheduling_{};
};

static_assert(sizeof(RuntimeGraph) == 88);

} // namespace bg
