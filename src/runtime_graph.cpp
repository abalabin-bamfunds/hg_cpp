#include <bg/details/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/evaluation_profile.hpp>

#include <bg/allocator.hpp>
#include <bg/details/evaluation_scratch.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/inherited_time_series_section.hpp>
#include <bg/graph/details/node_scheduler.hpp>
#include <bg/graph/details/service_binding_section.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/queue.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>

namespace bg {

struct RuntimeGraph::QueueState {
    details::QueueControl control;
    details::QueueMessageReader reader{util::buffer::concurrent_chain(
        4096, 64, allocator{*std::pmr::new_delete_resource()})};
};

details::NodeScheduler::NodeScheduler(const EvaluationContext &context) noexcept
    : graph_{context.runtime_graph_}, node_index_{context.node_index_} {}

void details::NodeScheduler::schedule() const noexcept {
    graph_->schedule(node_index_);
}

EvaluationContext details::NodeScheduler::node_context(EvaluationContext &parent,
                                                       const RuntimeGraphInput *inputs) const noexcept {
    auto context = parent.child(*graph_, inputs);
    context.node_index_ = node_index_;
    return context;
}

OutputChangeContext::OutputChangeContext(RuntimeGraph &runtime_graph) noexcept
    : runtime_graph_{&runtime_graph}, evaluation_time_{runtime_graph.evaluation_time()} {}

OutputChangeContext::OutputChangeContext(const EvaluationContext &evaluation_context) noexcept
    : runtime_graph_{&evaluation_context.runtime_graph()}, evaluation_time_{evaluation_context.evaluation_time()},
      runtime_graph_inputs_{evaluation_context.runtime_graph_inputs()} {}

ScheduleHandle RuntimeGraphSchedulingContext::schedule_at(EvaluationTime time) const {
    return graph_->schedule_at(node_index_, time);
}

void RuntimeGraphSchedulingContext::cancel(ScheduleHandle handle) const noexcept {
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
        static constexpr std::uintptr_t call_on_stop_flag = 1;

        std::uintptr_t owner_and_flags;
        void *receiver;
        void (*function)(void *, EvaluationContext &) noexcept;

        RuntimeGraph &owner() const noexcept {
            return *reinterpret_cast<RuntimeGraph *>(owner_and_flags & ~call_on_stop_flag);
        }
        bool call_on_stop() const noexcept { return (owner_and_flags & call_on_stop_flag) != 0; }
    };
    static_assert(sizeof(Record) == 3 * sizeof(void *));

  public:
    explicit CycleCallbacks(allocator memory) : records_{&memory.memory_resource()} {}

    void add(RuntimeGraph &owner, void *receiver, void (*function)(void *, EvaluationContext &) noexcept,
             bool call_on_stop) {
        const auto address = reinterpret_cast<std::uintptr_t>(&owner);
        assert((address & Record::call_on_stop_flag) == 0);
        records_.push_back({address | (call_on_stop ? Record::call_on_stop_flag : 0), receiver, function});
    }

    void remove(const void *receiver) noexcept {
        std::erase_if(records_, [=](const auto &record) { return record.receiver == receiver; });
    }

    void remove(RuntimeGraph &owner, EvaluationContext &context) noexcept {
        const auto *inputs = &context.runtime_graph() == &owner ? context.runtime_graph_inputs() : nullptr;
        auto callback_context = context.child(owner, inputs);
        for (auto record = records_.rbegin(); record != records_.rend(); ++record) {
            if (&record->owner() == &owner && record->call_on_stop()) {
                record->function(record->receiver, callback_context);
            }
        }
        std::erase_if(records_, [&](const auto &record) {
            return &record.owner() == &owner;
        });
    }

    void run(EvaluationContext &context) noexcept {
        const auto count = records_.size();
        for (auto i = count; i != 0; --i) {
            const auto record = records_[i - 1];
            const auto &owner = record.owner();
            const auto *inputs = &context.runtime_graph() == &owner ? context.runtime_graph_inputs() : nullptr;
            auto callback_context = context.child(record.owner(), inputs);
            record.function(record.receiver, callback_context);
        }
        records_.erase(records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(count));
    }

    void clear(EvaluationContext &context) noexcept {
        for (auto record = records_.rbegin(); record != records_.rend(); ++record) {
            if (record->call_on_stop()) {
                const auto &owner = record->owner();
                const auto *inputs = &context.runtime_graph() == &owner ? context.runtime_graph_inputs() : nullptr;
                auto callback_context = context.child(record->owner(), inputs);
                record->function(record->receiver, callback_context);
            }
        }
        records_.clear();
    }

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
                auto &scheduling = nodes_->graph_->scheduling_info(word_index_);
                const auto evaluation = scheduling.evaluation;
                const auto validity_check = scheduling.validity_check;
                const auto pending = (evaluation | validity_check) & ~processed_;

                if (pending == 0) {
                    scheduling.evaluation = ScheduleWord{};
                    scheduling.validity_check = ScheduleWord{};
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

    explicit ScheduledNodes(RuntimeGraph &graph) noexcept
        : graph_(&graph), word_count_{graph.compiled_graph_->schedule_word_count()} {}

    auto begin() noexcept -> Iterator { return Iterator{*this}; }
    auto begin(std::size_t node_index) noexcept -> Iterator {
        const auto word_index = node_index / CompiledGraph::schedule_word_bits;
        const auto bit = node_index % CompiledGraph::schedule_word_bits;
        const auto current_mask = ScheduleWord{1} << bit;
        return Iterator{*this, word_index, current_mask - 1};
    }
    auto end() const noexcept -> std::default_sentinel_t { return {}; }

  private:
    std::size_t word_count() const noexcept { return word_count_; }

    RuntimeGraph *graph_;
    std::size_t word_count_;
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
      evaluator_{allocator_.new_object<RuntimeGraphEvaluator>()}, instance_id_{GraphInstanceIdAllocator::allocate()},
      root_global_state_{&global_state},
      owns_evaluator_{true}, owns_storage_{true} {
    try {
        initialize_storage();
    } catch (...) {
        allocator_.delete_object(evaluator_);
        throw;
    }
}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, RuntimeGraphEvaluator &evaluator,
                           RuntimeGraphSchedulingCallbacks::token_type owner_token)
    : compiled_graph_(&compiled_graph), allocator_(memory), evaluator_{&evaluator},
      instance_id_{GraphInstanceIdAllocator::allocate()}, owner_token_{owner_token},
      owns_storage_{true} {
    assert(evaluator.owner_bound());
    initialize_storage();
}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, std::span<std::byte> storage,
                           GlobalState &global_state,
                           std::optional<CompiledGraph::ScheduleWord> previous_input_validity)
    : RuntimeGraph(compiled_graph, memory, storage, global_state, GraphInstanceIdAllocator::allocate(),
                   previous_input_validity) {}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, allocator memory, std::span<std::byte> storage,
                           GlobalState &global_state, GraphInstanceId instance_id,
                           std::optional<CompiledGraph::ScheduleWord> previous_input_validity)
    : compiled_graph_(&compiled_graph), allocator_(memory), evaluator_{}, instance_id_{instance_id},
      storage_{storage.data()}, root_global_state_{&global_state}, started_{true} {
    if (!compiled_graph.supports_borrowed_evaluation()) {
        throw std::invalid_argument{"the compiled graph does not support borrowed evaluation"};
    }
    if (storage.size() < borrowed_storage_size(compiled_graph)) {
        throw std::invalid_argument{"borrowed runtime storage is too small"};
    }
    if (!storage.empty() && reinterpret_cast<std::uintptr_t>(storage.data()) %
                                borrowed_storage_alignment(compiled_graph) !=
                            0) {
        throw std::invalid_argument{"borrowed runtime storage is misaligned"};
    }

    const auto initial_scheduling = compiled_graph.initial_scheduling_info();
    if (!initial_scheduling.empty()) {
        inline_scheduling_ = initial_scheduling.front();
    }
    if (previous_input_validity) {
        inline_scheduling_.clear_requests();
        inline_scheduling_.input_validity(*previous_input_validity);
        for (auto word_index = std::size_t{1}; word_index < compiled_graph.schedule_word_count(); ++word_index) {
            scheduling_info(word_index).clear_requests();
        }
    }
}

GlobalState &RuntimeGraph::global_state() const noexcept {
    assert(owns_evaluator_ || !owns_storage_);
    return *root_global_state_;
}

std::size_t RuntimeGraph::borrowed_storage_size(const CompiledGraph &compiled_graph) noexcept {
    const auto runtime_storage_size = compiled_graph.runtime_storage_size();
    const auto scheduling_word_count = compiled_graph.schedule_word_count();
    if (scheduling_word_count <= 1) {
        return runtime_storage_size;
    }

    const auto spill_count = scheduling_word_count - 1;
    const auto spill_offset =
        (runtime_storage_size + alignof(CompiledGraph::SchedulingInfo) - 1) &
        ~(alignof(CompiledGraph::SchedulingInfo) - 1);
    return spill_offset + spill_count * sizeof(CompiledGraph::SchedulingInfo);
}

std::size_t RuntimeGraph::borrowed_storage_alignment(const CompiledGraph &compiled_graph) noexcept {
    auto alignment = compiled_graph.runtime_storage_alignment();
    if (compiled_graph.schedule_word_count() > 1) {
        alignment = std::max(alignment, alignof(CompiledGraph::SchedulingInfo));
    }
    return alignment;
}

void RuntimeGraph::initialize_borrowed_storage(const CompiledGraph &compiled_graph, std::span<std::byte> storage) {
    if (!compiled_graph.supports_borrowed_evaluation()) {
        throw std::invalid_argument{"the compiled graph does not support borrowed evaluation"};
    }
    if (storage.size() < borrowed_storage_size(compiled_graph)) {
        throw std::invalid_argument{"borrowed runtime storage is too small"};
    }
    if (!storage.empty() && reinterpret_cast<std::uintptr_t>(storage.data()) %
                                borrowed_storage_alignment(compiled_graph) !=
                            0) {
        throw std::invalid_argument{"borrowed runtime storage is misaligned"};
    }

    const auto runtime_storage_size = compiled_graph.runtime_storage_size();
    if (runtime_storage_size != 0) {
        std::memcpy(storage.data(), compiled_graph.initial_runtime_storage(), runtime_storage_size);
    }

    const auto initial_scheduling = compiled_graph.initial_scheduling_info();
    if (initial_scheduling.size() > 1) {
        const auto spill_offset = (runtime_storage_size + alignof(CompiledGraph::SchedulingInfo) - 1) &
                                  ~(alignof(CompiledGraph::SchedulingInfo) - 1);
        auto *spill = reinterpret_cast<CompiledGraph::SchedulingInfo *>(storage.data() + spill_offset);
        std::uninitialized_copy(initial_scheduling.begin() + 1, initial_scheduling.end(), spill);
    }
}

void RuntimeGraph::initialize_storage() {
    const auto initial_scheduling = compiled_graph_->initial_scheduling_info();
    if (!initial_scheduling.empty()) {
        inline_scheduling_ = initial_scheduling.front();
    }

    if (runtime_allocation_size() == 0) {
        return;
    }

    storage_ = static_cast<std::byte *>(
        allocator_.allocate_bytes(runtime_allocation_size(), runtime_allocation_alignment()));
    // The compiled image contains only default node states whose runtime types support byte-copy initialization.
    const auto runtime_storage_size = compiled_graph_->runtime_storage_size();
    if (runtime_storage_size != 0) {
        std::memcpy(storage_, compiled_graph_->initial_runtime_storage(), runtime_storage_size);
    }

    if (initial_scheduling.size() > 1) {
        const auto offset = (runtime_storage_size + alignof(SchedulingInfo) - 1) & ~(alignof(SchedulingInfo) - 1);
        auto *spill = reinterpret_cast<SchedulingInfo *>(storage_ + offset);
        std::uninitialized_copy(initial_scheduling.begin() + 1, initial_scheduling.end(), spill);
    }
}

std::size_t RuntimeGraph::runtime_allocation_size() const noexcept {
    return borrowed_storage_size(*compiled_graph_);
}

std::size_t RuntimeGraph::runtime_allocation_alignment() const noexcept {
    return borrowed_storage_alignment(*compiled_graph_);
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

void RuntimeGraph::notify_runtime_graph_inputs(EvaluationContext &evaluation_context) {
    const auto bindings = compiled_graph_->borrowed_input_bindings();
    if (bindings.empty()) {
        return;
    }
    const auto *inputs = evaluation_context.runtime_graph_inputs();
    auto context = OutputChangeContext{evaluation_context};

    for (auto index = std::size_t{}; index < bindings.size(); ++index) {
        const auto &input = inputs[index];
        if (input.change == RuntimeGraphInputChange::unchanged) {
            continue;
        }

        auto change = OutputChange::update;
        switch (input.change) {
        case RuntimeGraphInputChange::update:
            break;
        case RuntimeGraphInputChange::invalidation:
            change = OutputChange::invalidation;
            break;
        case RuntimeGraphInputChange::revalidation:
            change = OutputChange::revalidation;
            break;
        case RuntimeGraphInputChange::unchanged:
            continue;
        }
        if (const auto *notification = compiled_graph_->borrowed_input_notification(bindings[index])) {
            notification->notify(context, input.runtime_type, input.value, change);
        }
        notify_subscribers(compiled_graph_->borrowed_input_subscribers(bindings[index]), change);
    }
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
    } else if (owns_storage_) {
        assert(finalized_);
    }

    if (compiled_graph_->has_timed_scheduling() && timed_schedule() != nullptr) {
        allocator_.delete_object(timed_schedule());
    }

    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        allocator_.delete_object(cycle_callbacks());
    }

    if (compiled_graph_->has_queue() && queue_state() != nullptr) {
        allocator_.delete_object(queue_state());
    }

    if (owns_storage_ && storage_ != nullptr) {
        allocator_.deallocate_bytes(storage_, runtime_allocation_size(), runtime_allocation_alignment());
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

    if (!owns_storage_) {
        return;
    }

    close_queue();
    clear_cycle_callbacks(parent_context);
    discard_queue();
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
                allocator_, context.global_state().scalar_allocation_pool(), this, node_index};
            node.state_destruction_function()(node, storage_, &context);
        }

        const auto output_destruction = compiled_graph_->output_destruction();
        for (auto i = output_destruction.offsets.size(); i != 0; --i) {
            output_destruction.runtime_types[i - 1]->destroy_value(storage_ + output_destruction.offsets[i - 1],
                                                                   allocator_);
        }
    }
}

void RuntimeGraph::schedule(std::size_t node_index) noexcept {
    const auto word_index = node_index / CompiledGraph::schedule_word_bits;
    auto &word = scheduling_info(word_index).evaluation;
    word |= ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);

    notify_owner_scheduled();
}

void RuntimeGraph::schedule_validity_check(std::size_t node_index) noexcept {
    auto &word = scheduling_info(node_index / CompiledGraph::schedule_word_bits).validity_check;
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
    return *reinterpret_cast<TimedSchedule **>(storage_);
}

RuntimeGraph::TimedSchedule *RuntimeGraph::timed_schedule() const noexcept {
    return *reinterpret_cast<TimedSchedule *const *>(storage_);
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
        auto context = RuntimeGraphSchedulingContext{*parent_, owner_node_index_};
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
    if (evaluator_ != nullptr) {
        evaluator_->notify_scheduled(*this);
    }
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
                                          const RuntimeGraphSchedulingCallbacks *callbacks) {
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

void RuntimeGraphEvaluator::bind_owner(RuntimeGraph &graph, std::size_t node_index) {
    require_owner(graph, node_index, nullptr, nullptr);
}

void RuntimeGraphEvaluator::bind_owner(RuntimeGraph &graph, std::size_t node_index,
                                       RuntimeGraphSchedulingHandler handler) {
    if (handler.receiver == nullptr || handler.callbacks == nullptr || handler.callbacks->inner_scheduled == nullptr ||
        handler.callbacks->timed_schedule_changed == nullptr) {
        throw std::invalid_argument{"an inner-graph scheduling handler requires scheduling callbacks"};
    }
    require_owner(graph, node_index, handler.receiver, handler.callbacks);
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
        static_cast<const TimeSeriesValueRuntimeType &>(compiled_node.output_runtime_type());
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

RuntimeGraph::QueueState *&RuntimeGraph::queue_state() noexcept {
    return *reinterpret_cast<QueueState **>(storage_ + compiled_graph_->queue_offset());
}

void RuntimeGraph::close_queue() noexcept {
    if (compiled_graph_->has_queue() && queue_state() != nullptr) {
        queue_state()->control.close();
    }
}

void RuntimeGraph::discard_queue() noexcept {
    if (!compiled_graph_->has_queue() || queue_state() == nullptr) {
        return;
    }

    while (const auto target = queue_state()->reader.next_target()) {
        const auto &compiled_node = compiled_graph_->node(*target);
        const auto &runtime_type = compiled_node.output_runtime_type();
        queue_state()->reader.discard(runtime_type.type(), compiled_node.queue_mode());
    }
}

void RuntimeGraph::drain_queue(EvaluationContext &context) {
    if (!compiled_graph_->has_queue() || queue_state() == nullptr) {
        return;
    }

    auto &reader = queue_state()->reader;
    while (const auto target = reader.next_target()) {
        const auto &compiled_node = compiled_graph_->node(*target);
        const auto runtime_node = node(compiled_node);
        context.node_index_ = *target;
        const auto allocation_context = details::ScalarAllocationContext{
            allocator_, context.global_state().bundle_allocation_owner(), this, *target};
        auto output = OutputState{compiled_node.output_runtime_type()};
        output.bind(runtime_node.output_last_updated(), runtime_node.output_value(), context);
        const auto restore_context = util::scope_exit{[&]() noexcept {
            context.reset_scratch();
            context.node_index_ = 0;
        }};
        if (!reader.apply(output, compiled_node.queue_mode())) {
            return;
        }

        if (runtime_node.output_last_updated().time() == evaluation_time_) {
            schedule(*target);
        }
    }
}

Queue RuntimeGraph::make_queue(std::size_t target) {
    if (!compiled_graph_->has_queue()) {
        throw std::logic_error{"the compiled graph does not declare queue use"};
    }
    if (evaluator_ == nullptr || evaluator_->owner_bound()) {
        throw std::logic_error{"only an outer runtime graph can create a queue"};
    }
    if (!started_ || finalized_) {
        throw std::logic_error{"a queue requires a started runtime graph that is not finalized"};
    }
    const auto &compiled_node = compiled_graph_->node(target);
    if (stopped_ && queue_state() == nullptr) {
        return {};
    }
    if (queue_state() == nullptr) {
        queue_state() = allocator_.new_object<QueueState>();
    }

    return queue_state()->control.make_queue(queue_state()->reader.current_block(), target,
                                              compiled_node.output_runtime_type(), compiled_node.queue_mode());
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

void RuntimeGraph::add_cycle_callback(void *receiver, void (*function)(void *, EvaluationContext &) noexcept,
                                     bool call_on_stop) {
    if (stopped_) {
        return;
    }
    outer_graph().require_cycle_callbacks().add(*this, receiver, function, call_on_stop);
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

void RuntimeGraph::clear_cycle_callbacks(EvaluationContext &context) noexcept {
    auto &outer = outer_graph();
    if (&outer != this) {
        if (outer.compiled_graph_->has_cycle_callbacks() && outer.cycle_callbacks() != nullptr) {
            outer.cycle_callbacks()->remove(*this, context);
        }
        return;
    }
    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        cycle_callbacks()->clear(context);
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

std::optional<OutputChange> RuntimeGraph::current_cycle_output_change(const CompiledNode &node) const noexcept {
    const auto &last_updated = Node{node, storage_}.output_last_updated();
    if (last_updated.time() != evaluation_time_) {
        return std::nullopt;
    }

    if (last_updated.valid()) {
        return last_updated.became_valid() ? OutputChange::revalidation : OutputChange::update;
    }
    return OutputChange::invalidation;
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
                                      EvaluationContext &context) {
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
    context.node_index_ = node_index;
    const auto allocation_context = details::ScalarAllocationContext{
        allocator_, context.global_state().scalar_allocation_pool(), this, node_index};
    node.invalidation_function()(node, storage_, &context);
    context.node_index_ = 0;

    if (node.has_output()) {
        if (const auto change = output_change(node, output_was_valid)) {
            publish_output(node, *change);
        }
    }
}

void RuntimeGraph::start(EvaluationTime evaluation_time, const RuntimeGraphInput *inputs) {
    start(evaluation_time, global_state(), inputs);
}

void RuntimeGraph::start(EvaluationTime evaluation_time, GlobalState &global_state, const RuntimeGraphInput *inputs) {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time, *this, global_state, scratch, SystemWallClock::instance()};
    start(context, inputs);
}

void RuntimeGraph::start(EvaluationTime evaluation_time, EvaluationProfile &profile,
                         std::optional<Scalar<>> label, const RuntimeGraphInput *inputs) {
    start(evaluation_time, global_state(), profile, std::move(label), inputs);
}

void RuntimeGraph::start(EvaluationTime evaluation_time, GlobalState &global_state, EvaluationProfile &profile,
                         std::optional<Scalar<>> label, const RuntimeGraphInput *inputs) {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context =
        EvaluationContext{evaluation_time, *this, global_state, scratch, SystemWallClock::instance(), &profile};
    start(context, inputs, std::move(label));
}

void RuntimeGraph::start(EvaluationContext &parent_context, const RuntimeGraphInput *inputs,
                         std::optional<Scalar<>> label) {
    if (evaluator_ == nullptr) {
        throw std::logic_error{"a borrowed runtime graph view cannot be started"};
    }
    evaluator_->start(*this, parent_context, inputs, std::move(label));
}

void RuntimeGraphEvaluator::start(RuntimeGraph &graph, EvaluationContext &parent_context,
                                  const RuntimeGraphInput *inputs, std::optional<Scalar<>> label) {
    if (graph.started_ || graph.stopped_) {
        throw std::logic_error{"a runtime graph can only be started once"};
    }
    if (&parent_context.runtime_graph() == &graph &&
        !TimeSeriesState::can_represent(parent_context.evaluation_time())) {
        throw std::invalid_argument{"evaluation time cannot be represented by a time series"};
    }
    auto *profile = parent_context.evaluation_profile();
    if (&parent_context.runtime_graph() != &graph) {
        auto *parent_evaluator = parent_context.runtime_graph().evaluator_;
        if (parent_evaluator != nullptr && parent_evaluator->profile_ != profile) {
            throw std::logic_error{"a nested runtime graph must use its parent's evaluation profile"};
        }
    }
    if (profile_ != nullptr && profile_ != profile) {
        throw std::logic_error{"runtime graphs owned by one node must use the same evaluation profile"};
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
    auto context = EvaluationContext{graph, binding_context.context(), inputs};
    graph.started_ = true;
    graph.evaluation_time_ = context.evaluation_time();
    profile = context.evaluation_profile();
    profile_ = profile;
    if (profile != nullptr) {
        const auto parent_graph_id = parent_ ? parent_.get()->instance_id() : GraphInstanceId{};
        const auto owner_node_index = parent_ ? static_cast<std::uint32_t>(owner_node_index_)
                                              : std::numeric_limits<std::uint32_t>::max();
        profile->start_graph(graph.instance_id(), *graph.compiled_graph_, parent_graph_id, owner_node_index,
                             std::move(label));
    }
    const auto starts = graph.compiled_graph_->start_functions();
    auto started_node_count = std::size_t{};
    auto stop_on_failure = util::scope_exit{[&]() noexcept {
        graph.stopped_ = true;
        graph.stop_nodes(started_node_count, context);
        if (profile != nullptr) {
            profile->stop_graph(graph.instance_id());
        }
    }};

    for (auto index = std::size_t{}; index < inherited_references.size(); ++index) {
        graph.initialize_inherited_time_series(index, inherited_references[index]);
    }
    graph.notify_runtime_graph_inputs(context);
    if (const auto offset = graph.compiled_graph_->constant_start_time_offset()) {
        reinterpret_cast<TimeSeriesState *>(graph.storage_ + *offset)->publish(graph.evaluation_time_);
    }
    auto function = starts.functions.begin();

    for (const auto node_index : starts.nodes) {
        started_node_count = node_index;
        const auto &node = graph.compiled_graph_->node(node_index);
        const auto output_was_valid = node.has_output() && Node{node, graph.storage_}.output_last_updated().valid();

        context.node_index_ = node_index;
        const auto allocation_context = details::ScalarAllocationContext{
            graph.allocator_, context.global_state().scalar_allocation_pool(), &graph, node_index};
        (*function)(node, graph.storage_, &context);
        context.node_index_ = 0;
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
    close_queue();

    const auto stops = compiled_graph_->stop_functions();
    auto i = static_cast<std::size_t>(std::ranges::lower_bound(stops.nodes, node_count) - stops.nodes.begin());

    while (i != 0) {
        --i;
        const auto node_index = stops.nodes[i];

        const auto &node = compiled_graph_->node(node_index);
        parent_context.node_index_ = node_index;
        const auto allocation_context = details::ScalarAllocationContext{
            allocator_, parent_context.global_state().scalar_allocation_pool(), this, node_index};
        stops.functions[i](node, storage_, &parent_context);
        parent_context.node_index_ = 0;
    }

    clear_timed_schedule();
    clear_cycle_callbacks(parent_context);
    discard_queue();
}

void RuntimeGraph::stop(const RuntimeGraphInput *inputs) noexcept {
    stop(global_state(), inputs);
}

void RuntimeGraph::stop(GlobalState &global_state, const RuntimeGraphInput *inputs) noexcept {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time_, *this, global_state, scratch, SystemWallClock::instance()};
    stop(context, inputs);
}

void RuntimeGraph::stop(EvaluationProfile &profile, const RuntimeGraphInput *inputs) noexcept {
    stop(global_state(), profile, inputs);
}

void RuntimeGraph::stop(GlobalState &global_state, EvaluationProfile &profile,
                        const RuntimeGraphInput *inputs) noexcept {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context =
        EvaluationContext{evaluation_time_, *this, global_state, scratch, SystemWallClock::instance(), &profile};
    stop(context, inputs);
}

void RuntimeGraph::stop(EvaluationContext &parent_context, const RuntimeGraphInput *inputs) noexcept {
    if (evaluator_ == nullptr) {
        stopped_ = true;
        return;
    }
    evaluator_->stop(*this, parent_context, inputs);
}

void RuntimeGraphEvaluator::stop(RuntimeGraph &graph, EvaluationContext &parent_context,
                                 const RuntimeGraphInput *inputs) noexcept {
    if (!graph.started_ || graph.stopped_) {
        return;
    }

    auto run_context = EvaluationContext{parent_context, profile_};
    auto binding_context = ServiceBindingContext{run_context, graph.service_bindings()};
    auto context = EvaluationContext{graph, binding_context.context(), inputs};
    graph.stopped_ = true;
    graph.stop_nodes(graph.compiled_graph_->node_count(), context);
    if (profile_ != nullptr) {
        profile_->stop_graph(graph.instance_id());
    }
}

bool RuntimeGraph::inputs_are_valid(const CompiledNode &node, const EvaluationContext &context) const {
    const auto runtime_node = Node{node, storage_};
    const auto borrowed_bindings = compiled_graph_->borrowed_input_bindings();
    const auto *runtime_graph_inputs = borrowed_bindings.empty() ? nullptr : context.runtime_graph_inputs();
    for (auto parameter_index = std::size_t{}; parameter_index < runtime_node.input_count(); ++parameter_index) {
        const auto &parameter = node.parameters()[parameter_index];
        if (parameter.mode() == ParameterBindingMode::scalar || parameter.validity() == optional) {
            continue;
        }

        if (!runtime_node.input_last_updated(parameter_index, runtime_graph_inputs).valid()) {
            return false;
        }
    }

    for (const auto &descriptor : node.all_valid_input_descriptors()) {
        const auto input = runtime_node.input_data(descriptor.parameter_index, runtime_graph_inputs);
        const auto &runtime_type = runtime_node.all_valid_input_runtime_type(descriptor, context);
        if (!runtime_type.all_valid(*input.last_updated, input.value, *descriptor.declared_type)) {
            return false;
        }
    }

    return true;
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time, const RuntimeGraphInput *inputs) {
    evaluate(evaluation_time, global_state(), inputs);
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time, GlobalState &global_state,
                            const RuntimeGraphInput *inputs) {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context = EvaluationContext{evaluation_time, *this, global_state, scratch, SystemWallClock::instance()};
    if (!evaluate(context, inputs)) {
        throw std::logic_error{"a pending evaluation escaped the outer graph"};
    }
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time, EvaluationProfile &profile,
                            const RuntimeGraphInput *inputs) {
    evaluate(evaluation_time, global_state(), profile, inputs);
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time, GlobalState &global_state,
                            EvaluationProfile &profile, const RuntimeGraphInput *inputs) {
    auto scratch = details::EvaluationScratch{allocator_.memory_resource()};
    auto context =
        EvaluationContext{evaluation_time, *this, global_state, scratch, SystemWallClock::instance(), &profile};
    if (!evaluate(context, inputs)) {
        throw std::logic_error{"a pending evaluation escaped the outer graph"};
    }
}

bool RuntimeGraph::evaluate(EvaluationContext &parent_context, const RuntimeGraphInput *inputs) {
    return RuntimeGraphEvaluator::evaluate(*this, parent_context, inputs);
}

bool RuntimeGraphEvaluator::evaluate(RuntimeGraph &graph, EvaluationContext &parent_context,
                                     const RuntimeGraphInput *inputs) {
    if (!graph.started_ || graph.stopped_) {
        throw std::logic_error{"a runtime graph must be running to evaluate"};
    }

    auto *profile = parent_context.evaluation_profile();
    if (graph.evaluator_ != nullptr && profile != graph.evaluator_->profile_) {
        throw std::logic_error{"runtime graph must use the evaluation profile selected at start"};
    }
    if (profile == nullptr) {
        return evaluate_graph<false>(graph, parent_context, nullptr, {}, inputs);
    }

    if (profile->failed()) {
        return evaluate_graph<false>(graph, parent_context, nullptr, {}, inputs);
    }

    const auto graph_id = graph.instance_id();
    if (graph_id.value == 0) {
        throw std::logic_error{"runtime graph has no evaluation profile identity"};
    }
    return evaluate_graph<true>(graph, parent_context, profile, graph_id, inputs);
}

template <bool Profiled>
bool RuntimeGraphEvaluator::evaluate_graph(RuntimeGraph &graph, EvaluationContext &parent_context,
                                           EvaluationProfile *profile, GraphInstanceId graph_id,
                                           const RuntimeGraphInput *inputs) {
    if (&parent_context.runtime_graph() == &graph &&
        !TimeSeriesState::can_represent(parent_context.evaluation_time())) {
        throw std::invalid_argument{"evaluation time cannot be represented by a time series"};
    }

    auto binding_context = ServiceBindingContext{parent_context, graph.service_bindings()};
    auto context = EvaluationContext{graph, binding_context.context(), inputs};
    const auto evaluation_time = context.evaluation_time();
    const auto resuming = graph.paused_node_index_ != std::numeric_limits<std::uint32_t>::max();
    const auto outer_graph_evaluation = graph.evaluator_ != nullptr && !graph.evaluator_->owner_bound();
    if (!resuming) {
        if (evaluation_time <= graph.last_evaluation_time_) {
            throw std::invalid_argument{"evaluation time must increase between outer cycles"};
        }
    } else if (evaluation_time != graph.evaluation_time_) {
        throw std::invalid_argument{"a paused runtime graph must resume in the same outer cycle"};
    }

    if constexpr (Profiled) {
        if (!outer_graph_evaluation) {
            profile->enter_graph(graph_id);
        }
        if (!resuming && outer_graph_evaluation) {
            profile->begin_cycle(graph_id, evaluation_time, context.wall_clock().now());
        }
    }

    auto leave_profiled_graph = util::scope_exit{[&]() noexcept {
        if constexpr (Profiled) {
            if (!outer_graph_evaluation) {
                profile->exit_graph(graph_id);
            }
        }
    }};

    auto abort_profile_cycle = util::scope_exit{[&]() noexcept {
        if constexpr (Profiled) {
            if (outer_graph_evaluation) {
                profile->abort_cycle();
            }
        }
    }};

    if (!resuming) {
        graph.run_cycle_callbacks(context);
        graph.last_evaluation_time_ = evaluation_time;
        graph.evaluation_time_ = evaluation_time;
        graph.notify_runtime_graph_inputs(context);
        graph.activate_scheduled(evaluation_time);
    }

    auto scheduled = RuntimeGraph::ScheduledNodes{graph};
    auto stop_on_failure = util::scope_exit{[&]() noexcept {
        graph.stopped_ = true;
        graph.stop_nodes(graph.compiled_graph_->node_count(), context);
    }};
    if (!resuming && outer_graph_evaluation) {
        graph.drain_queue(context);
    }

    for (auto scheduled_node = resuming ? scheduled.begin(graph.paused_node_index_) : scheduled.begin();
         scheduled_node != std::default_sentinel;) {
        const auto [node_index, evaluation_requested, validity_check_requested] = *scheduled_node;
        auto advance = util::scope_exit{[&]() noexcept { ++scheduled_node; }};
        const auto word_index = node_index / CompiledGraph::schedule_word_bits;
        const auto mask = RuntimeGraph::ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);
        const auto &node = graph.compiled_graph_->node(node_index);
        const auto has_output = node.has_output();
        auto &scheduling = graph.scheduling_info(word_index);
        const auto inputs_were_valid = scheduling.inputs_valid(mask);
        auto inputs_valid = inputs_were_valid;

        if (validity_check_requested ||
            (evaluation_requested && (!inputs_valid || node.has_all_valid_input_metadata()))) {
            inputs_valid = graph.inputs_are_valid(node, context);
            scheduling.inputs_valid(mask, inputs_valid);
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

        const auto evaluate = node.evaluation_function();
        const auto output_was_valid = evaluate != nullptr && has_output &&
                                      Node{node, graph.storage_}.output_last_updated().valid();
        if (evaluate != nullptr) {
            context.node_index_ = node_index;
            const auto allocation_context = details::ScalarAllocationContext{
                graph.allocator_, context.global_state().scalar_allocation_pool(), &graph, node_index};
            const auto evaluation_completed = evaluate(node, graph.storage_, &context);
            context.reset_scratch();
            context.node_index_ = 0;
            if (!evaluation_completed) {
                graph.paused_node_index_ = static_cast<std::uint32_t>(node_index);
                advance.release();
                stop_on_failure.release();
                if constexpr (Profiled) {
                    if (!outer_graph_evaluation) {
                        abort_profile_cycle.release();
                    }
                }
                return false;
            }
        }

        if constexpr (Profiled) {
            profile->record_node(node_index);
        }

        if (has_output) {
            const auto change = evaluate == nullptr ? graph.current_cycle_output_change(node)
                                                    : graph.output_change(node, output_was_valid);
            if (change) {
                graph.publish_output(node, *change);
            }
        }
    }
    stop_on_failure.release();
    graph.paused_node_index_ = std::numeric_limits<std::uint32_t>::max();
    if constexpr (Profiled) {
        if (outer_graph_evaluation) {
            profile->end_cycle(context.wall_clock().now());
        }
    }
    abort_profile_cycle.release();
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
                                           void (*function)(void *, EvaluationContext &) noexcept,
                                           bool call_on_stop) const {
    runtime_graph_->add_cycle_callback(receiver, function, call_on_stop);
}

GlobalState &EvaluationContext::global_state() const noexcept {
    return *global_state_;
}

EvaluationContext::EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph,
                                     std::size_t node_index, GlobalState &global_state,
                                     details::EvaluationScratch *evaluation_scratch, const WallClock &wall_clock,
                                     const details::EvaluationContextSection *sections,
                                     EvaluationProfile *evaluation_profile) noexcept
    : evaluation_time_(evaluation_time), wall_clock_(&wall_clock), runtime_graph_(&runtime_graph),
      node_index_(node_index), global_state_{&global_state}, scratch_{evaluation_scratch}, sections_(sections),
      evaluation_profile_{evaluation_profile} {}

EvaluationContext EvaluationContext::child(details::EvaluationContextSection &section) noexcept {
    section.parent = sections_;
    return EvaluationContext{*this, section};
}

EvaluationContext EvaluationContext::child(RuntimeGraph &runtime_graph, const RuntimeGraphInput *inputs) noexcept {
    return EvaluationContext{runtime_graph, *this, inputs};
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
