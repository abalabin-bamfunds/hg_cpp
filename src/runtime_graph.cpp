#include <bg/details/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>

#include <bg/allocator.hpp>
#include <bg/details/evaluation_scratch.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/inherited_time_series_section.hpp>
#include <bg/graph/details/node_scheduler.hpp>
#include <bg/graph/details/service_binding_section.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace bg {

details::NodeScheduler::NodeScheduler(const EvaluationContext &context) noexcept
    : graph_{context.runtime_graph_}, node_index_{context.node_index_} {}

void details::NodeScheduler::schedule() const noexcept {
    graph_->schedule(node_index_);
}

EvaluationContext details::NodeScheduler::node_context(EvaluationContext &parent) const noexcept {
    return EvaluationContext{parent, *graph_, node_index_};
}

OutputChangeContext::OutputChangeContext(RuntimeGraph &runtime_graph) noexcept
    : runtime_graph_{&runtime_graph}, evaluation_time_{runtime_graph.evaluation_time()} {}

OutputChangeContext::OutputChangeContext(const EvaluationContext &evaluation_context) noexcept
    : runtime_graph_{&evaluation_context.runtime_graph()}, evaluation_time_{evaluation_context.evaluation_time()} {}

ScheduleHandle RuntimeGraphOwnerContext::schedule_at(EvaluationTime time) const {
    return graph_->schedule_at(node_index_, time);
}

void RuntimeGraphOwnerContext::cancel(ScheduleHandle handle) const noexcept {
    graph_->cancel(node_index_, handle);
}

EvaluationTime OutputChangeContext::evaluation_time() const noexcept {
    return evaluation_time_;
}

void OutputChangeContext::schedule_validity_check(RuntimeGraph &graph, std::size_t node_index) const noexcept {
    graph.schedule_validity_check(node_index);
}

void OutputChangeContext::schedule(RuntimeGraph &graph, std::size_t node_index) const noexcept {
    graph.schedule(node_index);
}

void OutputChangeContext::notify_subscribers(details::DynamicSubscribers *subscribers, OutputChange change) const {
    if (subscribers != nullptr) {
        runtime_graph_->notify_dynamic_subscribers(*subscribers, change);
    }
}

void notify_input_subscribers(OutputChangeContext &context, const TimeSeriesRuntimeType *, const void *,
                              OutputChange change, std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const InputNotificationSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(InputNotificationSubscriber),
    };
    for (const auto &record : records) {
        record.notify(context, change);
    }
}

void notify_controlled_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *, const void *,
                              OutputChange change, std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const ControlledInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ControlledInputSubscriber),
    };
    for (const auto &record : records) {
        const auto active = *static_cast<bool *>(context.runtime_graph().storage_at(record.activity_offset()));
        if ((change != OutputChange::invalidation && active) ||
            (change == OutputChange::invalidation && active && !record.required())) {
            context.schedule(context.runtime_graph(), record.node_index());
        }
        if (change != OutputChange::update && record.required()) {
            context.schedule_validity_check(context.runtime_graph(), record.node_index());
        }
    }
}

class RuntimeGraph::TimedSchedule {
    struct Record {
        EvaluationTime time;
        std::uint64_t id;
        std::uint32_t node_index;
    };

    struct Later {
        bool operator()(const Record &left, const Record &right) const noexcept { return left.time > right.time; }
    };

  public:
    explicit TimedSchedule(allocator memory) : records_{&memory.memory_resource()} {}

    [[nodiscard]] std::uint64_t add(EvaluationTime time, std::size_t node_index) {
        const auto id = next_id_++;
        records_.push_back(Record{time, id, static_cast<std::uint32_t>(node_index)});
        std::ranges::push_heap(records_, Later{});

        return id;
    }

    void cancel(std::size_t node_index, std::uint64_t id) noexcept {
        const auto record = std::ranges::find_if(
            records_, [=](const auto &entry) { return entry.id == id && entry.node_index == node_index; });
        if (record != records_.end()) {
            records_.erase(record);
            std::ranges::make_heap(records_, Later{});
        }
    }

    void activate(RuntimeGraph &graph, EvaluationTime time) noexcept {
        while (!records_.empty() && records_.front().time <= time) {
            std::ranges::pop_heap(records_, Later{});
            graph.schedule(records_.back().node_index);
            records_.pop_back();
        }
    }

    std::optional<EvaluationTime> next_time() const noexcept {
        if (records_.empty()) {
            return std::nullopt;
        }

        return records_.front().time;
    }

    void clear() noexcept { records_.clear(); }

  private:
    std::pmr::vector<Record> records_;
    std::uint64_t next_id_{1};
};

class RuntimeGraph::CycleCallbacks {
    struct Record {
        RuntimeGraph *owner;
        void *receiver;
        void (*function)(void *, EvaluationContext &) noexcept;
    };

  public:
    explicit CycleCallbacks(allocator memory) : records_{&memory.memory_resource()} {}

    void add(RuntimeGraph &owner, void *receiver, void (*function)(void *, EvaluationContext &) noexcept) {
        records_.push_back({&owner, receiver, function});
    }

    void remove(const void *receiver) noexcept {
        std::erase_if(records_, [=](const auto &record) { return record.receiver == receiver; });
    }

    void remove(RuntimeGraph &owner) noexcept {
        std::erase_if(records_, [&](const auto &record) { return record.owner == &owner; });
    }

    void run(EvaluationContext &context) noexcept {
        const auto count = records_.size();
        for (auto i = count; i != 0; --i) {
            const auto record = records_[i - 1];
            auto callback_context = EvaluationContext{context, *record.owner, 0};
            record.function(record.receiver, callback_context);
        }
        records_.erase(records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(count));
    }

    void clear() noexcept { records_.clear(); }

  private:
    std::pmr::vector<Record> records_;
};

class ServiceBindingContext {
  public:
    ServiceBindingContext(EvaluationContext &parent, std::span<details::ServiceRequestBinding *const> bindings)
        : section_{bindings} {
        if (!bindings.empty()) {
            context_.emplace(parent.child(section_));
        }
        parent_ = &parent;
    }

    EvaluationContext &context() noexcept { return context_ ? *context_ : *parent_; }

  private:
    EvaluationContext *parent_{};
    details::ServiceBindingSection section_;
    std::optional<EvaluationContext> context_;
};

class RuntimeGraph::ScheduledNodes {
    // Iterates evaluation and validity requests together in node order. A scheduling block remains live until all
    // requests added while that block is current have been returned.

  public:
    struct ScheduledNode {
        std::size_t node_index;
        bool evaluation_requested;
        bool validity_check_requested;
    };

    class Iterator {
      public:
        explicit Iterator(ScheduledNodes &nodes, std::size_t word_index = 0, ScheduleWord processed = {}) noexcept
            : nodes_(&nodes), word_index_{word_index}, processed_{processed} {
            advance();
        }

        auto operator*() const noexcept -> ScheduledNode { return current_; }

        auto operator++() noexcept -> Iterator & {
            // Every lower node has already had its opportunity to run in this outer cycle.
            processed_ |= current_mask_ | (current_mask_ - 1);
            advance();
            return *this;
        }

        auto operator==(std::default_sentinel_t) const noexcept -> bool { return word_index_ == nodes_->word_count(); }

      private:
        void advance() noexcept {
            while (word_index_ < nodes_->word_count()) {
                const auto evaluation = nodes_->evaluation(word_index_);
                const auto validity_check = nodes_->validity_check(word_index_);
                const auto pending = (evaluation | validity_check) & ~processed_;

                if (pending == 0) {
                    nodes_->clear(word_index_);
                    ++word_index_;
                    processed_ = ScheduleWord{};
                    continue;
                }

                const auto bit = static_cast<std::size_t>(std::countr_zero(pending));
                const auto mask = ScheduleWord{1} << bit;

                current_ = {
                    .node_index = word_index_ * CompiledGraph::schedule_word_bits + bit,
                    .evaluation_requested = (evaluation & mask) != 0,
                    .validity_check_requested = (validity_check & mask) != 0,
                };
                current_mask_ = mask;
                return;
            }
        }

        ScheduledNodes *nodes_;
        std::size_t word_index_{};
        ScheduleWord processed_{};
        ScheduleWord current_mask_{};
        ScheduledNode current_{};
    };

    explicit ScheduledNodes(std::span<SchedulingInfo> scheduling) noexcept : scheduling_(scheduling) {}

    auto begin() noexcept -> Iterator { return Iterator{*this}; }
    auto begin(std::size_t node_index) noexcept -> Iterator {
        const auto word_index = node_index / CompiledGraph::schedule_word_bits;
        const auto bit = node_index % CompiledGraph::schedule_word_bits;
        const auto current_mask = ScheduleWord{1} << bit;
        return Iterator{*this, word_index, current_mask - 1};
    }
    auto end() const noexcept -> std::default_sentinel_t { return {}; }

  private:
    std::size_t word_count() const noexcept { return scheduling_.size(); }

    ScheduleWord evaluation(std::size_t index) const noexcept { return scheduling_[index].evaluation; }

    ScheduleWord validity_check(std::size_t index) const noexcept { return scheduling_[index].validity_check; }

    void clear(std::size_t index) noexcept {
        scheduling_[index].evaluation = ScheduleWord{};
        scheduling_[index].validity_check = ScheduleWord{};
    }

    std::span<SchedulingInfo> scheduling_;
};

class RuntimeGraph::OutputChangePublisher {
    struct Scope {
        OutputChangePublisher *publisher;
        ItemSubscriptionRange subscriptions;
    };

  public:
    explicit OutputChangePublisher(RuntimeGraph &graph) noexcept : graph_{&graph} {}

    void publish(const CompiledNode &compiled_node, OutputChange change) {
        const auto runtime_node = Node{compiled_node, graph_->storage_};
        publish_modified_items(compiled_node, runtime_node);
        publish_handlers(compiled_node.output_notification(), nullptr, runtime_node.output_value(), change);
        graph_->notify_subscribers(compiled_node.subscribers(), change);
    }

  private:
    void publish_modified_items(const CompiledNode &compiled_node, Node runtime_node) {
        const auto *root = compiled_node.item_subscriptions();
        if (root == nullptr) {
            return;
        }

        auto scope = Scope{this, root->children()};
        root->runtime_type->for_each_modified_item(runtime_node.output_last_updated(), runtime_node.output_value(),
                                                   graph_->evaluation_time_, &scope, visit_modified_item);
    }

    static void visit_modified_item(void *context, TimeSeriesRuntimeType::ModifiedItem item) {
        auto &scope = *static_cast<Scope *>(context);
        const auto *subscribers = scope.subscriptions.find(item.id);
        if (subscribers == nullptr) {
            return;
        }

        scope.publisher->publish_item(item, *subscribers);
    }

    void publish_item(TimeSeriesRuntimeType::ModifiedItem item, const ItemSubscription &subscription) {
        auto &sampled_last_updated =
            *reinterpret_cast<TimeSeriesState *>(graph_->storage_ + subscription.sampled_last_updated_offset());
        const auto was_valid = sampled_last_updated.valid();
        const auto now_valid = item.last_updated->valid();

        if (was_valid || now_valid) {
            if (now_valid) {
                sampled_last_updated.publish(graph_->evaluation_time_);
            } else {
                sampled_last_updated.invalidate(graph_->evaluation_time_);
            }
            const auto change = now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation)
                                          : OutputChange::invalidation;
            publish_handlers(subscription.output_notification(), item.runtime_type, item.value, change);
            graph_->notify_subscribers(subscription.subscribers(), change);
        }

        auto scope = Scope{this, subscription.children()};
        item.runtime_type->for_each_modified_item(*item.last_updated, item.value, graph_->evaluation_time_, &scope,
                                                  visit_modified_item);
    }

    void publish_handlers(const OutputNotification *handlers, const TimeSeriesRuntimeType *runtime_type,
                          const void *value, OutputChange change) {
        if (handlers == nullptr) {
            return;
        }

        auto context = OutputChangeContext{*graph_};
        handlers->notify(context, runtime_type, value, change);
    }

    RuntimeGraph *graph_;
};

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state)
    : RuntimeGraph(compiled_graph, allocator{}, global_state) {}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, GlobalState &global_state)
    : compiled_graph_(&compiled_graph), allocator_(memory),
      evaluator_{allocator_.new_object<RuntimeGraphEvaluator>()}, root_global_state_{&global_state},
      owns_evaluator_{true} {
    try {
        initialize_storage();
    } catch (...) {
        allocator_.delete_object(evaluator_);
        throw;
    }
}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, RuntimeGraphEvaluator &evaluator,
                           RuntimeGraphOwnerCallbacks::token_type owner_token)
    : compiled_graph_(&compiled_graph), allocator_(memory), evaluator_{&evaluator}, owner_token_{owner_token} {
    initialize_storage();
}

GlobalState &RuntimeGraph::global_state() const noexcept {
    assert(owns_evaluator_);
    return *root_global_state_;
}

void RuntimeGraph::initialize_storage() {
    if (compiled_graph_->runtime_storage_size() == 0) {
        return;
    }

    storage_ = static_cast<std::byte *>(allocator_.allocate_bytes(compiled_graph_->runtime_storage_size(),
                                                                  compiled_graph_->runtime_storage_alignment()));
    // The image contains only default states whose runtime types support byte-copy initialization.
    std::memcpy(storage_, compiled_graph_->initial_runtime_storage(), compiled_graph_->runtime_storage_size());
}

void RuntimeGraph::initialize_inherited_time_series(std::size_t index, Reference reference) {
    const auto requirements = compiled_graph_->inherited_time_series();
    if (index >= requirements.size()) {
        throw std::out_of_range{"the inherited time series index is out of range"};
    }

    const auto &binding = requirements[index];
    auto &stored_last_updated = *reinterpret_cast<TimeSeriesState *>(storage_ + binding.last_updated_offset);
    auto &stored_reference = *reinterpret_cast<Reference *>(storage_ + binding.value_offset);
    stored_reference = reference;
    if (!reference) {
        return;
    }
    stored_last_updated.publish(evaluation_time_);

    if (const auto *notification = compiled_graph_->inherited_time_series_notification(binding)) {
        auto context = OutputChangeContext{*this};
        notification->notify(context, nullptr, &stored_reference, OutputChange::revalidation);
    }
    notify_subscribers(compiled_graph_->inherited_time_series_subscribers(binding), OutputChange::revalidation);
}

std::span<details::ServiceRequestBinding *> RuntimeGraph::service_bindings() noexcept {
    const auto *metadata = compiled_graph_->service_binding_metadata();
    if (metadata == nullptr) {
        return {};
    }
    return {reinterpret_cast<details::ServiceRequestBinding **>(storage_ + metadata->runtime_bindings_offset),
            metadata->binding_count};
}

RuntimeGraph::~RuntimeGraph() {
    if (owns_evaluator_) {
        finalize(*root_global_state_);
    } else {
        assert(finalized_);
    }

    if (compiled_graph_->has_timed_scheduling() && timed_schedule() != nullptr) {
        allocator_.delete_object(timed_schedule());
    }

    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        allocator_.delete_object(cycle_callbacks());
    }

    if (storage_ != nullptr) {
        allocator_.deallocate_bytes(storage_, compiled_graph_->runtime_storage_size(),
                                    compiled_graph_->runtime_storage_alignment());
    }

    if (owns_evaluator_) {
        allocator_.delete_object(evaluator_);
    }
}

void RuntimeGraph::finalize(GlobalState &global_state) noexcept {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time_, *this, 0, global_state, &scratch,
                                     SystemWallClock::instance()};
    finalize(context);
}

void RuntimeGraph::finalize(EvaluationContext &parent_context) noexcept {
    if (finalized_) {
        return;
    }
    finalized_ = true;

    if (storage_ != nullptr) {
        for (auto node_index = compiled_graph_->node_count(); node_index != 0;) {
            const auto &node = compiled_graph_->node(--node_index);
            const auto runtime_node = Node{node, storage_};
            for (auto parameter_index = std::size_t{}; parameter_index < runtime_node.input_count();
                 ++parameter_index) {
                auto *input = runtime_node.reference_input(parameter_index);
                if (input == nullptr || !input->value) {
                    continue;
                }

                const auto offset = static_cast<std::size_t>(reinterpret_cast<std::byte *>(input) - storage_);
                details::ReferenceProvider::remove_subscriber(input->value, *this, offset);
            }
        }

        for (auto node_index = compiled_graph_->node_count(); node_index != 0;) {
            --node_index;
            const auto &node = compiled_graph_->node(node_index);
            if (!node.has_state() || node.state_destruction_function() == nullptr) {
                continue;
            }

            EvaluationContext context{parent_context, *this, node_index};
            const auto allocation_context = details::ScalarAllocationContext{
                allocator_, context.global_state().bundle_allocation_owner(), this, node_index};
            node.state_destruction_function()(node, storage_, &context);
        }

        const auto output_destruction = compiled_graph_->output_destruction();
        for (auto i = output_destruction.offsets.size(); i != 0; --i) {
            output_destruction.runtime_types[i - 1]->destroy_value(storage_ + output_destruction.offsets[i - 1],
                                                                   allocator_);
        }
    }
}

auto RuntimeGraph::scheduling_info() noexcept -> std::span<SchedulingInfo> {
    return {reinterpret_cast<SchedulingInfo *>(storage_), compiled_graph_->schedule_word_count()};
}

void RuntimeGraph::schedule(std::size_t node_index) noexcept {
    const auto word_index = node_index / CompiledGraph::schedule_word_bits;
    auto scheduling = scheduling_info();
    auto &word = scheduling[word_index].evaluation;
    word |= ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);

    notify_owner_scheduled();
}

void RuntimeGraph::schedule_validity_check(std::size_t node_index) noexcept {
    auto scheduling = scheduling_info();
    auto &word = scheduling[node_index / CompiledGraph::schedule_word_bits].validity_check;
    word |= ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);

    notify_owner_scheduled();
}

ScheduleHandle RuntimeGraph::schedule_at(std::size_t node_index, EvaluationTime time) {
    if (time <= evaluation_time_) {
        throw std::invalid_argument{"a scheduled evaluation time must be after the current evaluation time"};
    }

    if (!compiled_graph_->has_timed_scheduling()) {
        throw std::logic_error{"the graph does not declare timed scheduling"};
    }

    if (timed_schedule() == nullptr) {
        timed_schedule() = allocator_.new_object<TimedSchedule>(allocator_);
    }

    const auto previous = next_scheduled_time();
    auto result = ScheduleHandle{timed_schedule()->add(time, node_index)};
    notify_timed_schedule_changed(previous);
    return result;
}

void RuntimeGraph::cancel(std::size_t node_index, ScheduleHandle handle) noexcept {
    if (compiled_graph_->has_timed_scheduling() && timed_schedule() != nullptr && handle.id_ != 0) {
        const auto previous = next_scheduled_time();
        timed_schedule()->cancel(node_index, handle.id_);
        notify_timed_schedule_changed(previous);
    }
}

RuntimeGraph::TimedSchedule *&RuntimeGraph::timed_schedule() noexcept {
    return *reinterpret_cast<TimedSchedule **>(storage_ + compiled_graph_->timed_schedule_offset());
}

RuntimeGraph::TimedSchedule *RuntimeGraph::timed_schedule() const noexcept {
    return *reinterpret_cast<TimedSchedule *const *>(storage_ + compiled_graph_->timed_schedule_offset());
}

void RuntimeGraph::activate_scheduled(EvaluationTime evaluation_time) noexcept {
    if (compiled_graph_->has_timed_scheduling() && timed_schedule() != nullptr) {
        const auto previous = next_scheduled_time();
        timed_schedule()->activate(*this, evaluation_time);
        notify_timed_schedule_changed(previous);
    }
}

void RuntimeGraph::clear_timed_schedule() noexcept {
    if (compiled_graph_->has_timed_scheduling() && timed_schedule() != nullptr) {
        const auto previous = next_scheduled_time();
        timed_schedule()->clear();
        notify_timed_schedule_changed(previous);
    }
}

void RuntimeGraph::notify_timed_schedule_changed(std::optional<EvaluationTime> previous) {
    evaluator_->notify_timed_schedule_changed(*this, previous);
}

void RuntimeGraphEvaluator::notify_timed_schedule_changed(RuntimeGraph &graph,
                                                          std::optional<EvaluationTime> previous) {
    if (!parent_) {
        return;
    }

    const auto next = graph.next_scheduled_time();
    if (next == previous) {
        return;
    }

    if (parent_.tag()) {
        auto context = RuntimeGraphOwnerContext{*parent_, owner_node_index_};
        owner_.callback.callbacks->timed_schedule_changed(context, owner_.callback.receiver, graph.owner_token_, next);
        return;
    }

    if (owner_.direct.timed_schedule) {
        parent_->cancel(owner_node_index_, owner_.direct.timed_schedule);
        owner_.direct.timed_schedule = {};
    }
    if (next) {
        owner_.direct.timed_schedule = parent_->schedule_at(owner_node_index_, *next);
    }
}

void RuntimeGraph::notify_owner_scheduled() noexcept {
    evaluator_->notify_scheduled(*this);
}

void RuntimeGraphEvaluator::notify_scheduled(RuntimeGraph &graph) noexcept {
    if (!parent_ || graph.suppress_parent_scheduling_) {
        return;
    }

    if (parent_.tag()) {
        owner_.callback.callbacks->inner_scheduled(owner_.callback.receiver, graph.owner_token_);
    }
    parent_->schedule(owner_node_index_);
}

void RuntimeGraphEvaluator::require_owner(RuntimeGraph &graph, std::size_t node_index, void *receiver,
                                          const RuntimeGraphOwnerCallbacks *callbacks) {
    if (!parent_) {
        parent_.reset(&graph, callbacks != nullptr);
        owner_node_index_ = node_index;
        if (callbacks != nullptr) {
            owner_.callback = {.receiver = receiver, .callbacks = callbacks};
        } else {
            owner_.direct = {};
        }
        return;
    }

    if (parent_.get() != &graph || owner_node_index_ != node_index || parent_.tag() != (callbacks != nullptr) ||
        (callbacks != nullptr &&
         (owner_.callback.receiver != receiver || owner_.callback.callbacks != callbacks))) {
        throw std::logic_error{"a runtime graph evaluator cannot change owner"};
    }
}

void RuntimeGraphEvaluator::set_owner(RuntimeGraph &graph, std::size_t node_index) {
    require_owner(graph, node_index, nullptr, nullptr);
}

void RuntimeGraphEvaluator::set_owner(RuntimeGraph &graph, std::size_t node_index, const RuntimeGraphOwner &owner) {
    require_owner(graph, node_index, owner.receiver, owner.callbacks);
}

bool RuntimeGraph::set_owner_scheduling_suppressed(bool suppressed) noexcept {
    const auto previous = suppress_parent_scheduling_;
    suppress_parent_scheduling_ = suppressed;
    return previous;
}

bool RuntimeGraph::evaluation_pending() const noexcept {
    return paused_node_index_ != std::numeric_limits<std::uint32_t>::max();
}

void RuntimeGraph::update_reference_output(std::size_t node_index, Reference reference,
                                           EvaluationTime evaluation_time) {
    evaluation_time_ = evaluation_time;
    const auto &compiled_node = compiled_graph_->node(node_index);
    const auto runtime_node = node(compiled_node);
    auto &stored_last_updated = runtime_node.output_last_updated();
    auto &stored_reference = *static_cast<Reference *>(runtime_node.output_value());

    if (stored_reference == reference) {
        return;
    }

    const auto was_valid = stored_last_updated.valid();
    stored_reference = reference;
    if (reference) {
        stored_last_updated.publish(evaluation_time);
    } else if (was_valid) {
        stored_last_updated.invalidate(evaluation_time);
    } else {
        return;
    }
    const auto now_valid = stored_last_updated.valid();
    const auto change =
        now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation) : OutputChange::invalidation;
    publish_output(compiled_node, change);
}

void RuntimeGraph::initialize_constant_output(std::size_t node_index, Scalar<> value,
                                              EvaluationTime evaluation_time) {
    evaluation_time_ = evaluation_time;
    const auto &compiled_node = compiled_graph_->node(node_index);
    const auto runtime_node = node(compiled_node);
    const auto &runtime_type =
        static_cast<const TimeSeriesValueRuntimeType &>(compiled_node.node_invocation_output_runtime_type());
    runtime_type.scalar_runtime_type().assign_from_input(runtime_node.output_value(), value);
    runtime_node.output_last_updated().publish(evaluation_time);
    publish_output(compiled_node, OutputChange::revalidation);
}

RuntimeGraph::CycleCallbacks *&RuntimeGraph::cycle_callbacks() noexcept {
    return *reinterpret_cast<CycleCallbacks **>(storage_ + compiled_graph_->cycle_callbacks_offset());
}

RuntimeGraph::CycleCallbacks *RuntimeGraph::cycle_callbacks() const noexcept {
    return *reinterpret_cast<CycleCallbacks *const *>(storage_ + compiled_graph_->cycle_callbacks_offset());
}

RuntimeGraph &RuntimeGraph::outer_graph() noexcept {
    return evaluator_->outer_graph(*this);
}

RuntimeGraph &RuntimeGraphEvaluator::outer_graph(RuntimeGraph &graph) const noexcept {
    auto *result = &graph;
    while (result->evaluator_->parent_) {
        result = result->evaluator_->parent_.get();
    }
    return *result;
}

RuntimeGraph::CycleCallbacks &RuntimeGraph::require_cycle_callbacks() {
    if (!compiled_graph_->has_cycle_callbacks()) {
        throw std::logic_error{"the graph does not declare cycle callbacks"};
    }

    if (cycle_callbacks() == nullptr) {
        cycle_callbacks() = allocator_.new_object<CycleCallbacks>(allocator_);
    }

    return *cycle_callbacks();
}

void RuntimeGraph::add_cycle_callback(void *receiver, void (*function)(void *, EvaluationContext &) noexcept) {
    if (stopped_) {
        return;
    }
    outer_graph().require_cycle_callbacks().add(*this, receiver, function);
}

void RuntimeGraph::remove_cycle_callback(const void *receiver) noexcept {
    auto &outer = outer_graph();
    if (outer.compiled_graph_->has_cycle_callbacks() && outer.cycle_callbacks() != nullptr) {
        outer.cycle_callbacks()->remove(receiver);
    }
}

void RuntimeGraph::run_cycle_callbacks(EvaluationContext &context) noexcept {
    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        cycle_callbacks()->run(context);
    }
}

void RuntimeGraph::clear_cycle_callbacks() noexcept {
    auto &outer = outer_graph();
    if (&outer != this) {
        if (outer.compiled_graph_->has_cycle_callbacks() && outer.cycle_callbacks() != nullptr) {
            outer.cycle_callbacks()->remove(*this);
        }
        return;
    }
    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        cycle_callbacks()->clear();
    }
}

std::optional<EvaluationTime> RuntimeGraph::next_scheduled_time() const noexcept {
    if (!compiled_graph_->has_timed_scheduling() || timed_schedule() == nullptr) {
        return std::nullopt;
    }

    return timed_schedule()->next_time();
}

void RuntimeGraph::notify_subscribers(std::span<const Subscriber> subscribers, OutputChange change) {
    for (const auto &subscriber : subscribers) {
        notify_node_subscriber(subscriber.node_index(), subscriber.active(), subscriber.required(),
                               subscriber.active_optional(), change);
    }
}

void RuntimeGraph::notify_node_subscriber(std::size_t node_index, bool is_active, bool is_required,
                                          bool is_active_optional, OutputChange change) noexcept {
    if ((change != OutputChange::invalidation && is_active) ||
        (change == OutputChange::invalidation && is_active_optional)) {
        schedule(node_index);
    }

    if (change != OutputChange::update && is_required) {
        schedule_validity_check(node_index);
    }
}

std::optional<OutputChange> RuntimeGraph::output_change(const CompiledNode &node,
                                                        bool output_was_valid) const noexcept {
    auto &last_updated = Node{node, storage_}.output_last_updated();
    const auto now_valid = last_updated.valid();
    if (last_updated.time() != evaluation_time_ && output_was_valid == now_valid) {
        return std::nullopt;
    }

    return now_valid ? (output_was_valid ? OutputChange::update : OutputChange::revalidation)
                     : OutputChange::invalidation;
}

void RuntimeGraph::publish_output(const CompiledNode &node, OutputChange change) {
    OutputChangePublisher{*this}.publish(node, change);
}

void RuntimeGraph::notify_dynamic_subscribers(details::DynamicSubscribers &subscribers, OutputChange change) {
    subscribers.for_each_node_subscriber([&](const auto &subscriber) {
        auto active = subscriber.active();
        if (subscriber.controlled_activity()) {
            // Compilation places the dynamic activity byte directly after the reference input storage.
            const auto activity_offset = subscriber.input_offset + sizeof(ReferenceInputStorage);
            active = *static_cast<bool *>(subscriber.graph->storage_at(activity_offset));
        }
        subscriber.graph->notify_node_subscriber(subscriber.node_index(), active, subscriber.required(),
                                                 active && !subscriber.required(), change);
    });

    const auto notification_context = OutputChangeContext{*this};
    subscribers.for_each_input_subscriber([&](const auto &subscriber) {
        auto context = notification_context.for_graph(*subscriber.graph);
        subscriber.function(context, subscriber.node_index, subscriber.data, change);
    });

    subscribers.for_each_observer([&](const auto &observer) {
        auto context = notification_context.for_graph(*observer.graph);
        observer.function(context, observer.receiver, change);
    });
}

bool RuntimeGraph::dynamic_input_active(std::uint32_t input_offset) const noexcept {
    // Compilation places the dynamic activity byte directly after the reference input storage.
    const auto activity_offset = input_offset + sizeof(ReferenceInputStorage);
    return *reinterpret_cast<const bool *>(storage_ + activity_offset);
}

void RuntimeGraph::apply_invalidation(const CompiledNode &node, std::size_t node_index,
                                      EvaluationContext &parent_context) {
    switch (node.invalidation_mode()) {
    case InvalidationMode::invalidate_output:
        if (node.has_output()) {
            auto &last_updated = Node{node, storage_}.output_last_updated();
            if (last_updated.valid()) {
                last_updated.invalidate(evaluation_time_);
                publish_output(node, OutputChange::invalidation);
            }
        }
        return;

    case InvalidationMode::preserve_output:
        return;

    case InvalidationMode::invoke:
        break;
    }

    const auto output_was_valid = node.has_output() && Node{node, storage_}.output_last_updated().valid();
    EvaluationContext context{parent_context, *this, node_index};
    const auto allocation_context = details::ScalarAllocationContext{
        allocator_, context.global_state().bundle_allocation_owner(), this, node_index};
    node.invalidation_function()(node, storage_, &context);

    if (node.has_output()) {
        if (const auto change = output_change(node, output_was_valid)) {
            publish_output(node, *change);
        }
    }
}

void RuntimeGraph::start(EvaluationTime evaluation_time) {
    start(evaluation_time, global_state());
}

void RuntimeGraph::start(EvaluationTime evaluation_time, GlobalState &global_state) {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time, *this, global_state, scratch, SystemWallClock::instance()};
    start(context);
}

void RuntimeGraph::start(EvaluationContext &parent_context) {
    evaluator_->start(*this, parent_context);
}

void RuntimeGraphEvaluator::start(RuntimeGraph &graph, EvaluationContext &parent_context) {
    if (graph.started_ || graph.stopped_) {
        throw std::logic_error{"a runtime graph can only be started once"};
    }
    if (&parent_context.runtime_graph() == &graph &&
        !TimeSeriesState::can_represent(parent_context.evaluation_time())) {
        throw std::invalid_argument{"evaluation time cannot be represented by a time series"};
    }

    const auto inherited_time_series = graph.compiled_graph_->inherited_time_series();
    const auto *inherited_time_series_section =
        inherited_time_series.empty() ? nullptr
                                      : static_cast<const details::InheritedTimeSeriesSection *>(
                                            parent_context.find_section(&details::inherited_time_series_section_type));
    const auto inherited_references = inherited_time_series_section == nullptr
                                          ? std::span<const Reference>{}
                                          : inherited_time_series_section->references;
    if (inherited_references.size() != inherited_time_series.size()) {
        throw std::invalid_argument{"the runtime graph received the wrong number of inherited time series"};
    }

    auto bindings = graph.service_bindings();
    const auto provider_nodes = graph.compiled_graph_->service_binding_provider_nodes();
    const auto *binding_section = bindings.empty()
                                      ? nullptr
                                      : static_cast<const details::ServiceBindingSection *>(
                                            parent_context.find_section(&details::service_binding_section_type));
    const auto inherited_bindings =
        binding_section == nullptr ? std::span<details::ServiceRequestBinding *const>{} : binding_section->bindings;
    if (!bindings.empty() && provider_nodes.front() == details::no_service_binding_provider_node) {
        if (inherited_bindings.size() != bindings.size()) {
            throw std::invalid_argument{"the runtime graph received the wrong number of service bindings"};
        }
        std::ranges::copy(inherited_bindings, bindings.begin());
    } else if (!inherited_bindings.empty()) {
        throw std::invalid_argument{"a root runtime graph cannot receive service bindings"};
    } else {
        for (auto index = std::size_t{}; index < bindings.size(); ++index) {
            if (provider_nodes[index] == details::no_service_binding_provider_node) {
                throw std::invalid_argument{"a runtime graph mixes provided and inherited service bindings"};
            }
            bindings[index] =
                static_cast<details::ServiceRequestBinding *>(graph.node(provider_nodes[index]).state());
        }
    }

    auto binding_context = ServiceBindingContext{parent_context, bindings};
    auto &context = binding_context.context();

    graph.started_ = true;
    graph.evaluation_time_ = context.evaluation_time();
    const auto starts = graph.compiled_graph_->start_functions();
    auto started_node_count = std::size_t{};
    auto stop_on_failure = util::scope_exit{[&]() noexcept {
        graph.stopped_ = true;
        graph.stop_nodes(started_node_count, context);
    }};

    for (auto index = std::size_t{}; index < inherited_references.size(); ++index) {
        graph.initialize_inherited_time_series(index, inherited_references[index]);
    }
    if (const auto offset = graph.compiled_graph_->constant_start_time_offset()) {
        reinterpret_cast<TimeSeriesState *>(graph.storage_ + *offset)->publish(graph.evaluation_time_);
    }
    auto function = starts.functions.begin();

    for (const auto node_index : starts.nodes) {
        started_node_count = node_index;
        const auto &node = graph.compiled_graph_->node(node_index);
        const auto output_was_valid = node.has_output() && Node{node, graph.storage_}.output_last_updated().valid();

        EvaluationContext node_context{context, graph, node_index};
        const auto allocation_context = details::ScalarAllocationContext{
            graph.allocator_, context.global_state().bundle_allocation_owner(), &graph, node_index};
        (*function)(node, graph.storage_, &node_context);
        ++function;
        started_node_count = node_index + 1;

        if (node.has_output()) {
            if (const auto change = graph.output_change(node, output_was_valid)) {
                graph.publish_output(node, *change);
            }
        }
    }
    {
        auto context = OutputChangeContext{graph};
        for (const auto &notification : graph.compiled_graph_->constant_notifications()) {
            notification.notify(context, OutputChange::revalidation);
        }
    }
    stop_on_failure.release();
}

void RuntimeGraph::stop_nodes(std::size_t node_count, EvaluationContext &parent_context) noexcept {
    const auto stops = compiled_graph_->stop_functions();
    auto i = static_cast<std::size_t>(std::ranges::lower_bound(stops.nodes, node_count) - stops.nodes.begin());

    while (i != 0) {
        --i;
        const auto node_index = stops.nodes[i];

        const auto &node = compiled_graph_->node(node_index);
        EvaluationContext context{parent_context, *this, node_index};
        const auto allocation_context = details::ScalarAllocationContext{
            allocator_, context.global_state().bundle_allocation_owner(), this, node_index};
        stops.functions[i](node, storage_, &context);
    }

    clear_timed_schedule();
    clear_cycle_callbacks();
}

void RuntimeGraph::stop() noexcept {
    stop(global_state());
}

void RuntimeGraph::stop(GlobalState &global_state) noexcept {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time_, *this, global_state, scratch, SystemWallClock::instance()};
    stop(context);
}

void RuntimeGraph::stop(EvaluationContext &parent_context) noexcept {
    evaluator_->stop(*this, parent_context);
}

void RuntimeGraphEvaluator::stop(RuntimeGraph &graph, EvaluationContext &parent_context) noexcept {
    if (!graph.started_ || graph.stopped_) {
        return;
    }

    auto binding_context = ServiceBindingContext{parent_context, graph.service_bindings()};
    auto &context = binding_context.context();
    graph.stopped_ = true;

    const auto stops = graph.compiled_graph_->stop_functions();
    auto i = stops.nodes.size();
    while (i != 0) {
        --i;
        const auto node_index = stops.nodes[i];
        const auto &node = graph.compiled_graph_->node(node_index);
        EvaluationContext node_context{context, graph, node_index};
        const auto allocation_context = details::ScalarAllocationContext{
            graph.allocator_, context.global_state().bundle_allocation_owner(), &graph, node_index};
        stops.functions[i](node, graph.storage_, &node_context);
    }

    graph.clear_timed_schedule();
    graph.clear_cycle_callbacks();
}

bool RuntimeGraph::inputs_are_valid(const CompiledNode &node) const noexcept {
    const auto runtime_node = Node{node, storage_};
    for (auto parameter_index = std::size_t{}; parameter_index < runtime_node.input_count(); ++parameter_index) {
        const auto &parameter = node.parameters()[parameter_index];
        if (parameter.mode() == ParameterBindingMode::scalar || parameter.validity() != required) {
            continue;
        }

        if (!runtime_node.input_last_updated(parameter_index).valid()) {
            return false;
        }
    }

    return true;
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time) {
    evaluate(evaluation_time, global_state());
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time, GlobalState &global_state) {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time, *this, global_state, scratch, SystemWallClock::instance()};
    if (!evaluate(context)) {
        throw std::logic_error{"a pending evaluation escaped the outer graph"};
    }
}

bool RuntimeGraph::evaluate(EvaluationContext &parent_context) {
    return evaluator_->evaluate(*this, parent_context);
}

bool RuntimeGraphEvaluator::evaluate(RuntimeGraph &graph, EvaluationContext &parent_context) {
    if (!graph.started_ || graph.stopped_) {
        throw std::logic_error{"a runtime graph must be running to evaluate"};
    }

    if (&parent_context.runtime_graph() == &graph &&
        !TimeSeriesState::can_represent(parent_context.evaluation_time())) {
        throw std::invalid_argument{"evaluation time cannot be represented by a time series"};
    }

    auto binding_context = ServiceBindingContext{parent_context, graph.service_bindings()};
    auto &context = binding_context.context();
    const auto evaluation_time = context.evaluation_time();
    const auto resuming = graph.paused_node_index_ != std::numeric_limits<std::uint32_t>::max();
    if (!resuming) {
        if (evaluation_time <= graph.last_evaluation_time_) {
            throw std::invalid_argument{"evaluation time must increase between outer cycles"};
        }

        graph.run_cycle_callbacks(context);
        graph.last_evaluation_time_ = evaluation_time;
        graph.evaluation_time_ = evaluation_time;
        graph.activate_scheduled(evaluation_time);
    } else if (evaluation_time != graph.evaluation_time_) {
        throw std::invalid_argument{"a paused runtime graph must resume in the same outer cycle"};
    }

    auto scheduling = graph.scheduling_info();
    auto scheduled = RuntimeGraph::ScheduledNodes{scheduling};
    auto stop_on_failure = util::scope_exit{[&]() noexcept {
        graph.stopped_ = true;
        graph.stop_nodes(graph.compiled_graph_->node_count(), context);
    }};

    for (auto scheduled_node = resuming ? scheduled.begin(graph.paused_node_index_) : scheduled.begin();
         scheduled_node != std::default_sentinel;) {
        const auto [node_index, evaluation_requested, validity_check_requested] = *scheduled_node;
        auto advance = util::scope_exit{[&]() noexcept { ++scheduled_node; }};
        const auto word_index = node_index / CompiledGraph::schedule_word_bits;
        const auto mask = RuntimeGraph::ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);
        const auto &node = graph.compiled_graph_->node(node_index);
        const auto has_output = node.has_output();
        const auto inputs_were_valid = scheduling[word_index].inputs_valid(mask);
        auto inputs_valid = inputs_were_valid;

        if (validity_check_requested || (evaluation_requested && !inputs_valid)) {
            inputs_valid = graph.inputs_are_valid(node);
            scheduling[word_index].inputs_valid(mask, inputs_valid);
        }

        if (inputs_were_valid && !inputs_valid) {
            graph.apply_invalidation(node, node_index, context);
        }

        if (!inputs_valid) {
            continue;
        }

        if (!evaluation_requested) {
            continue;
        }

        const auto output_was_valid = has_output && Node{node, graph.storage_}.output_last_updated().valid();
        if (const auto evaluate = node.evaluation_function(); evaluate != nullptr) {
            EvaluationContext node_context{context, graph, node_index};
            const auto allocation_context = details::ScalarAllocationContext{
                graph.allocator_, context.global_state().bundle_allocation_owner(), &graph, node_index};
            if (!evaluate(node, graph.storage_, &node_context)) {
                graph.paused_node_index_ = static_cast<std::uint32_t>(node_index);
                advance.release();
                stop_on_failure.release();
                return false;
            }
        }

        if (has_output) {
            if (const auto change = graph.output_change(node, output_was_valid)) {
                graph.publish_output(node, *change);
            }
        }
    }
    stop_on_failure.release();
    graph.paused_node_index_ = std::numeric_limits<std::uint32_t>::max();
    return true;
}

void EvaluationContext::schedule_now() const noexcept {
    runtime_graph_->schedule(node_index_);
}

ScheduleHandle EvaluationContext::schedule_at(EvaluationTime time) const {
    return runtime_graph_->schedule_at(node_index_, time);
}

void EvaluationContext::cancel(ScheduleHandle handle) const noexcept {
    runtime_graph_->cancel(node_index_, handle);
}

void EvaluationContext::add_cycle_callback(void *receiver,
                                           void (*function)(void *, EvaluationContext &) noexcept) const {
    runtime_graph_->add_cycle_callback(receiver, function);
}

GlobalState &EvaluationContext::global_state() const noexcept {
    return *global_state_;
}

EvaluationContext::EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph,
                                     std::size_t node_index, GlobalState &global_state,
                                     details::EvaluationScratch *evaluation_scratch, const WallClock &wall_clock,
                                     const details::EvaluationContextSection *sections) noexcept
    : evaluation_time_(evaluation_time), wall_clock_(&wall_clock), runtime_graph_(&runtime_graph),
      node_index_(node_index), global_state_{&global_state}, scratch_{evaluation_scratch}, sections_(sections) {}

EvaluationContext EvaluationContext::child(details::EvaluationContextSection &section) noexcept {
    section.parent = sections_;
    return EvaluationContext{*this, section};
}

const details::EvaluationContextSection *EvaluationContext::find_section(const void *type,
                                                                         std::size_t depth) const noexcept {
    auto *section = sections_;
    while (section != nullptr) {
        if (section->type == type) {
            if (depth == 0) {
                return section;
            }
            --depth;
        }
        section = section->parent;
    }
    return nullptr;
}

EvaluationContext::~EvaluationContext() = default;

} // namespace bg
