#include <bg/graph/runtime_graph.hpp>

#include <bg/details/evaluation_scratch.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace bg {

void *OutputNotificationContext::runtime_storage_at(std::size_t offset) const noexcept {
    return runtime_graph_->storage_ + offset;
}

EvaluationTime OutputNotificationContext::evaluation_time() const noexcept {
    return runtime_graph_->evaluation_time_;
}

Node OutputNotificationContext::node(std::size_t node_index) const noexcept {
    return runtime_graph_->node(node_index);
}

std::pmr::memory_resource &OutputNotificationContext::memory_resource(RuntimeGraph &graph) const noexcept {
    return *graph.memory_;
}

void OutputNotificationContext::schedule_validity_check(RuntimeGraph &graph, std::size_t node_index) const noexcept {
    graph.schedule_validity_check(node_index);
}

void OutputNotificationContext::notify(details::DynamicSubscribers *subscribers, OutputChange change) const {
    if (subscribers != nullptr) {
        runtime_graph_->schedule_dynamic_subscribers(*subscribers, change);
    }
}

void notify_callback_inputs(OutputNotificationContext &context, const void *, OutputChange change,
                            std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const CallbackInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(CallbackInputSubscriber),
    };
    for (const auto &record : records) {
        record.notify(context, change);
    }
}

class RuntimeGraph::FutureSchedule {
    struct Record {
        EvaluationTime time;
        std::uint64_t id;
        std::uint32_t node_index;
    };

    struct Later {
        bool operator()(const Record &left, const Record &right) const noexcept {
            return left.time > right.time;
        }
    };

  public:
    explicit FutureSchedule(std::pmr::memory_resource &memory) : records_{&memory} {}

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
        void *target;
        void (*function)(void *) noexcept;
    };

  public:
    explicit CycleCallbacks(std::pmr::memory_resource &memory) : records_{&memory} {}

    void add(void *target, void (*function)(void *) noexcept) { records_.push_back({target, function}); }

    void run() noexcept {
        const auto count = records_.size();
        for (auto i = count; i != 0; --i) {
            records_[i - 1].function(records_[i - 1].target);
        }
        records_.erase(records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(count));
    }

    void clear() noexcept { records_.clear(); }

  private:
    std::pmr::vector<Record> records_;
};

class RuntimeGraph::ScratchUse {
  public:
    ScratchUse(RuntimeGraph &graph, details::EvaluationScratch &scratch) noexcept
        : graph_{&graph}, previous_{graph.scratch_} {
        graph.scratch_ = &scratch;
    }

    ~ScratchUse() { graph_->scratch_ = previous_; }

  private:
    RuntimeGraph *graph_;
    details::EvaluationScratch *previous_;
};

struct RuntimeGraph::PendingInputNotifications {
    // A target graph applies cross-graph input reactions after entering the source's outer cycle.
    struct Record {
        InputNotificationFunction input_function;
        std::size_t node_index;
        std::size_t data;
        OutputChange change;
    };

    explicit PendingInputNotifications(std::pmr::memory_resource &memory) : records{&memory} {}

    std::pmr::vector<Record> records;
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
        explicit Iterator(ScheduledNodes &nodes) noexcept : nodes_(&nodes) { advance(); }

        auto operator*() const noexcept -> ScheduledNode { return current_; }

        auto operator++() noexcept -> Iterator & {
            advance();
            return *this;
        }

        auto operator==(std::default_sentinel_t) const noexcept -> bool {
            return word_index_ == nodes_->word_count();
        }

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
                processed_ |= mask;
                return;
            }
        }

        ScheduledNodes *nodes_;
        std::size_t word_index_{};
        ScheduleWord processed_{};
        ScheduledNode current_{};
    };

    explicit ScheduledNodes(std::span<SchedulingInfo> scheduling) noexcept : scheduling_(scheduling) {}

    auto begin() noexcept -> Iterator { return Iterator{*this}; }
    auto end() const noexcept -> std::default_sentinel_t { return {}; }

  private:
    std::size_t word_count() const noexcept { return scheduling_.size(); }

    ScheduleWord evaluation(std::size_t index) const noexcept { return scheduling_[index].evaluation; }

    ScheduleWord validity_check(std::size_t index) const noexcept {
        return scheduling_[index].validity_check;
    }

    void clear(std::size_t index) noexcept {
        scheduling_[index].evaluation = ScheduleWord{};
        scheduling_[index].validity_check = ScheduleWord{};
    }

    std::span<SchedulingInfo> scheduling_;
};

class RuntimeGraph::ModifiedItemPropagator {
    struct Scope {
        ModifiedItemPropagator *propagator;
        ItemSubscriptionRange subscriptions;
    };

  public:
    explicit ModifiedItemPropagator(RuntimeGraph &graph) noexcept : graph_{&graph} {}

    void propagate(const CompiledNode &node) {
        const auto *root = node.item_subscriptions();
        if (root == nullptr) {
            return;
        }

        const auto runtime_node = Node{node, graph_->storage_};
        auto scope = Scope{this, root->children()};
        root->runtime_type->for_each_modified_item(runtime_node.output_last_updated(), runtime_node.output_value(),
                                                   graph_->evaluation_time_, &scope, propagate_item);
    }

  private:
    static void propagate_item(void *context, TimeSeriesRuntimeType::ModifiedItem item) {
        auto &scope = *static_cast<Scope *>(context);
        const auto *subscribers = scope.subscriptions.find(item.id);
        if (subscribers == nullptr) {
            return;
        }

        scope.propagator->propagate(item, *subscribers);
    }

    void propagate(TimeSeriesRuntimeType::ModifiedItem item, const ItemSubscribers &subscribers) {
        auto &sampled_last_updated =
            *reinterpret_cast<EvaluationTime *>(graph_->storage_ + subscribers.sampled_last_updated_offset());
        const auto was_valid = sampled_last_updated > MIN_DT;
        const auto now_valid = *item.last_updated > MIN_DT;

        if (was_valid || now_valid) {
            sampled_last_updated = now_valid ? graph_->evaluation_time_ : MIN_DT;
            const auto change = now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation)
                                          : OutputChange::invalidation;
            graph_->notify_output(subscribers.output_notification(), item.value, change);
            graph_->schedule_subscribers(subscribers.subscribers(), change);
        }

        auto scope = Scope{this, subscribers.children()};
        item.runtime_type->for_each_modified_item(*item.last_updated, item.value, graph_->evaluation_time_, &scope,
                                                  propagate_item);
    }

    RuntimeGraph *graph_;
};

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph)
    : RuntimeGraph(compiled_graph, *std::pmr::get_default_resource()) {}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory)
    : compiled_graph_(&compiled_graph), memory_(&memory), owned_global_state_(std::make_unique<GlobalState>()),
      global_state_(owned_global_state_.get()) {
    initialize_storage();
}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, GlobalState &global_state)
    : RuntimeGraph(compiled_graph, *std::pmr::get_default_resource(), global_state) {}

RuntimeGraph::RuntimeGraph(const CompiledGraph &compiled_graph, std::pmr::memory_resource &memory,
                           GlobalState &global_state)
    : compiled_graph_(&compiled_graph), memory_(&memory), global_state_(&global_state) {
    initialize_storage();
}

void RuntimeGraph::initialize_storage() {
    if (compiled_graph_->runtime_storage_size() == 0) {
        return;
    }

    storage_ = static_cast<std::byte *>(
        memory_->allocate(compiled_graph_->runtime_storage_size(), compiled_graph_->runtime_storage_alignment()));
    // The image contains only default states whose runtime types support byte-copy initialization.
    std::memcpy(storage_, compiled_graph_->initial_runtime_storage(), compiled_graph_->runtime_storage_size());
}

RuntimeGraph::~RuntimeGraph() {
    if (parent_ != nullptr && parent_schedule_time_) {
        parent_->cancel(owner_node_index_, parent_schedule_);
    }

    if (storage_ != nullptr) {
        for (auto node_index = compiled_graph_->node_count(); node_index != 0;) {
            const auto &node = compiled_graph_->node(--node_index);
            const auto runtime_node = Node{node, storage_};
            for (auto parameter_index = std::size_t{}; parameter_index < runtime_node.input_count();
                 ++parameter_index) {
                const auto &parameter = node.parameters()[parameter_index];
                if (parameter.mode() != ParameterBindingMode::reference) {
                    continue;
                }

                auto &input = runtime_node.reference_input(parameter_index);
                if (input.target) {
                    details::remove_reference_subscriber(input.target, *this, parameter.value_offset());
                }
            }
        }

        for (auto node_index = compiled_graph_->node_count(); node_index != 0;) {
            --node_index;
            const auto &node = compiled_graph_->node(node_index);
            if (!node.has_state() || node.state_destruction_function() == nullptr) {
                continue;
            }

            EvaluationContext context{evaluation_time_, *this, node_index};
            const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
            node.state_destruction_function()(node, storage_, &context);
        }

        const auto output_destruction = compiled_graph_->output_destruction();
        for (auto i = output_destruction.offsets.size(); i != 0; --i) {
            output_destruction.runtime_types[i - 1]->destroy_value(storage_ + output_destruction.offsets[i - 1],
                                                                   *memory_);
        }
    }

    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr) {
        std::destroy_at(future_schedule());
        memory_->deallocate(future_schedule(), sizeof(FutureSchedule), alignof(FutureSchedule));
    }

    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        std::destroy_at(cycle_callbacks());
        memory_->deallocate(cycle_callbacks(), sizeof(CycleCallbacks), alignof(CycleCallbacks));
    }

    if (pending_input_notifications_ != nullptr) {
        std::destroy_at(pending_input_notifications_);
        memory_->deallocate(pending_input_notifications_, sizeof(PendingInputNotifications),
                            alignof(PendingInputNotifications));
    }

    if (storage_ != nullptr) {
        memory_->deallocate(storage_, compiled_graph_->runtime_storage_size(),
                            compiled_graph_->runtime_storage_alignment());
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

    if (parent_ != nullptr && !suppress_parent_scheduling_) {
        parent_->schedule(owner_node_index_);
    }
}

void RuntimeGraph::schedule_validity_check(std::size_t node_index) noexcept {
    auto scheduling = scheduling_info();
    auto &word = scheduling[node_index / CompiledGraph::schedule_word_bits].validity_check;
    word |= ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);

    if (parent_ != nullptr && !suppress_parent_scheduling_) {
        parent_->schedule(owner_node_index_);
    }
}

ScheduleHandle RuntimeGraph::schedule_at(std::size_t node_index, EvaluationTime time) {
    if (time <= evaluation_time_) {
        throw std::invalid_argument{"a scheduled evaluation time must be in the future"};
    }

    if (!compiled_graph_->has_future_scheduling()) {
        throw std::logic_error{"the graph does not declare future scheduling"};
    }

    if (future_schedule() == nullptr) {
        auto *storage = memory_->allocate(sizeof(FutureSchedule), alignof(FutureSchedule));
        auto release = util::scope_exit{
            [&]() noexcept { memory_->deallocate(storage, sizeof(FutureSchedule), alignof(FutureSchedule)); }};
        future_schedule() = std::construct_at(static_cast<FutureSchedule *>(storage), *memory_);
        release.release();
    }

    auto result = ScheduleHandle{future_schedule()->add(time, node_index)};
    mirror_future_schedule();
    return result;
}

void RuntimeGraph::cancel(std::size_t node_index, ScheduleHandle handle) noexcept {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr && handle.id_ != 0) {
        future_schedule()->cancel(node_index, handle.id_);
        mirror_future_schedule();
    }
}

RuntimeGraph::FutureSchedule *&RuntimeGraph::future_schedule() noexcept {
    return *reinterpret_cast<FutureSchedule **>(storage_ + compiled_graph_->future_schedule_offset());
}

RuntimeGraph::FutureSchedule *RuntimeGraph::future_schedule() const noexcept {
    return *reinterpret_cast<FutureSchedule *const *>(storage_ + compiled_graph_->future_schedule_offset());
}

void RuntimeGraph::activate_scheduled(EvaluationTime evaluation_time) noexcept {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr) {
        future_schedule()->activate(*this, evaluation_time);
        mirror_future_schedule();
    }
}

void RuntimeGraph::clear_future_schedule() noexcept {
    if (compiled_graph_->has_future_scheduling() && future_schedule() != nullptr) {
        future_schedule()->clear();
        mirror_future_schedule();
    }
}

void RuntimeGraph::mirror_future_schedule() {
    if (parent_ == nullptr) {
        return;
    }

    const auto next = next_scheduled_time();
    if (next == parent_schedule_time_) {
        return;
    }

    if (parent_schedule_time_) {
        parent_->cancel(owner_node_index_, parent_schedule_);
        parent_schedule_ = {};
        parent_schedule_time_.reset();
    }

    if (next) {
        parent_schedule_ = parent_->schedule_at(owner_node_index_, *next);
        parent_schedule_time_ = next;
    }
}

RuntimeGraph::CycleCallbacks *&RuntimeGraph::cycle_callbacks() noexcept {
    return *reinterpret_cast<CycleCallbacks **>(storage_ + compiled_graph_->cycle_callbacks_offset());
}

RuntimeGraph::CycleCallbacks *RuntimeGraph::cycle_callbacks() const noexcept {
    return *reinterpret_cast<CycleCallbacks *const *>(storage_ + compiled_graph_->cycle_callbacks_offset());
}

void RuntimeGraph::add_cycle_callback(void *target, void (*function)(void *) noexcept) {
    if (!compiled_graph_->has_cycle_callbacks()) {
        throw std::logic_error{"the graph does not declare cycle callbacks"};
    }

    if (cycle_callbacks() == nullptr) {
        auto *storage = memory_->allocate(sizeof(CycleCallbacks), alignof(CycleCallbacks));
        auto release = util::scope_exit{
            [&]() noexcept { memory_->deallocate(storage, sizeof(CycleCallbacks), alignof(CycleCallbacks)); }};
        cycle_callbacks() = std::construct_at(static_cast<CycleCallbacks *>(storage), *memory_);
        release.release();
    }

    cycle_callbacks()->add(target, function);
}

void RuntimeGraph::run_cycle_callbacks() noexcept {
    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        cycle_callbacks()->run();
    }
}

void RuntimeGraph::clear_cycle_callbacks() noexcept {
    if (compiled_graph_->has_cycle_callbacks() && cycle_callbacks() != nullptr) {
        cycle_callbacks()->clear();
    }
}

std::optional<EvaluationTime> RuntimeGraph::next_scheduled_time() const noexcept {
    if (!compiled_graph_->has_future_scheduling() || future_schedule() == nullptr) {
        return std::nullopt;
    }

    return future_schedule()->next_time();
}

void RuntimeGraph::schedule_subscribers(std::span<const Subscriber> subscribers, OutputChange change) {
    for (const auto &subscriber : subscribers) {
        if (subscriber.has_dynamic_subscribers()) {
            schedule_dynamic_subscribers(subscriber.dynamic_subscribers_offset(), change);
            continue;
        }

        schedule_node_subscriber(subscriber.node_index(), subscriber.active(), subscriber.required(),
                                 subscriber.active_optional(), change);
    }
}

void RuntimeGraph::schedule_node_subscriber(std::size_t node_index, bool is_active, bool is_required,
                                            bool is_active_optional, OutputChange change) noexcept {
    if ((change != OutputChange::invalidation && is_active) ||
        (change == OutputChange::invalidation && is_active_optional)) {
        schedule(node_index);
    }

    if (change != OutputChange::update && is_required) {
        schedule_validity_check(node_index);
    }
}

void RuntimeGraph::notify_output(const OutputNotification *notification, const void *value, OutputChange change) {
    if (notification == nullptr) {
        return;
    }

    auto context = OutputNotificationContext{*this};
    notification->notify(context, value, change);
}

void RuntimeGraph::propagate_output_change(const CompiledNode &node, OutputChange change) {
    const auto runtime_node = Node{node, storage_};
    propagate_modified_items(node);
    notify_output(node.output_notification(), runtime_node.output_value(), change);
    schedule_subscribers(node.subscribers(), change);
}

void RuntimeGraph::publish_output_changes(const CompiledNode &node, bool output_was_valid) {
    auto &last_updated = Node{node, storage_}.output_last_updated();
    const auto now_valid = last_updated > MIN_DT;
    if (last_updated != evaluation_time_ && output_was_valid == now_valid) {
        return;
    }

    propagate_output_change(node, now_valid ? (output_was_valid ? OutputChange::update : OutputChange::revalidation)
                                            : OutputChange::invalidation);
}

void RuntimeGraph::propagate_modified_items(const CompiledNode &node) {
    ModifiedItemPropagator{*this}.propagate(node);
}

void RuntimeGraph::schedule_dynamic_subscribers(std::size_t offset, OutputChange change) {
    auto *subscribers = *reinterpret_cast<details::DynamicSubscribers **>(storage_ + offset);
    if (subscribers == nullptr) {
        return;
    }

    schedule_dynamic_subscribers(*subscribers, change);
}

void RuntimeGraph::schedule_dynamic_subscribers(details::DynamicSubscribers &subscribers, OutputChange change) {
    subscribers.for_each([&](const auto &subscriber) {
        subscriber.graph->schedule_node_subscriber(subscriber.node_index(), subscriber.active(), subscriber.required(),
                                                   subscriber.active_optional(), change);
    });

    subscribers.for_each_callback([&](const auto &subscriber) {
        if (subscriber.graph == this) {
            auto context = OutputNotificationContext{*this};
            subscriber.callback(context, subscriber.node_index, subscriber.data, change);
            return;
        }

        subscriber.graph->queue_input_notification(subscriber.callback, subscriber.node_index, subscriber.data, change);
    });

    subscribers.for_each_observer([&](const auto &observer) {
        auto context = OutputNotificationContext{*this};
        observer.function(context, observer.target, change);
    });
}

void RuntimeGraph::queue_input_notification(InputNotificationFunction function, std::size_t node_index,
                                            std::size_t data, OutputChange change) {
    if (pending_input_notifications_ == nullptr) {
        auto *storage = memory_->allocate(sizeof(PendingInputNotifications), alignof(PendingInputNotifications));
        auto release = util::scope_exit{[&]() noexcept {
            memory_->deallocate(storage, sizeof(PendingInputNotifications), alignof(PendingInputNotifications));
        }};
        pending_input_notifications_ = std::construct_at(static_cast<PendingInputNotifications *>(storage), *memory_);
        release.release();
    }

    pending_input_notifications_->records.push_back({function, node_index, data, change});
    if (parent_ != nullptr && !suppress_parent_scheduling_) {
        parent_->schedule(owner_node_index_);
    }
}

void RuntimeGraph::run_input_notifications() {
    if (pending_input_notifications_ == nullptr || pending_input_notifications_->records.empty()) {
        return;
    }

    auto records = std::move(pending_input_notifications_->records);
    pending_input_notifications_->records.clear();
    auto context = OutputNotificationContext{*this};
    for (const auto &record : records) {
        record.input_function(context, record.node_index, record.data, record.change);
    }
}

void RuntimeGraph::apply_invalidation(const CompiledNode &node, std::size_t node_index) {
    switch (node.invalidation_mode()) {
    case InvalidationMode::invalidate_output:
        if (node.has_output()) {
            auto &last_updated = Node{node, storage_}.output_last_updated();
            if (last_updated > MIN_DT) {
                last_updated = MIN_DT;
                propagate_output_change(node, OutputChange::invalidation);
            }
        }
        return;

    case InvalidationMode::preserve_output:
        return;

    case InvalidationMode::invoke:
        break;
    }

    const auto output_was_valid = node.has_output() && Node{node, storage_}.output_last_updated() > MIN_DT;
    EvaluationContext context{evaluation_time_, *this, node_index};
    const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
    node.invalidation_function()(node, storage_, &context);

    if (node.has_output()) {
        publish_output_changes(node, output_was_valid);
    }
}

void RuntimeGraph::start(EvaluationTime evaluation_time) {
    auto scratch = details::EvaluationScratch{*memory_};
    start(evaluation_time, scratch);
}

void RuntimeGraph::start(EvaluationTime evaluation_time, details::EvaluationScratch &scratch) {
    if (started_ || stopped_) {
        throw std::logic_error{"a runtime graph can only be started once"};
    }

    started_ = true;
    const auto scratch_use = ScratchUse{*this, scratch};
    evaluation_time_ = evaluation_time;
    const auto starts = compiled_graph_->start_functions();
    auto started_node_count = std::size_t{};
    auto function = starts.functions.begin();
    auto stop_on_failure = util::scope_exit{[&]() noexcept {
        stopped_ = true;
        stop_nodes(started_node_count);
    }};

    for (const auto node_index : starts.nodes) {
        started_node_count = node_index;
        const auto &node = compiled_graph_->node(node_index);
        const auto output_was_valid = node.has_output() && Node{node, storage_}.output_last_updated() > MIN_DT;

        EvaluationContext context{evaluation_time, *this, node_index};
        const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
        (*function)(node, storage_, &context);
        ++function;
        started_node_count = node_index + 1;

        if (node.has_output()) {
            publish_output_changes(node, output_was_valid);
        }
    }
    stop_on_failure.release();
}

void RuntimeGraph::stop_nodes(std::size_t node_count) noexcept {
    const auto stops = compiled_graph_->stop_functions();
    auto i = static_cast<std::size_t>(std::ranges::lower_bound(stops.nodes, node_count) - stops.nodes.begin());

    while (i != 0) {
        --i;
        const auto node_index = stops.nodes[i];

        const auto &node = compiled_graph_->node(node_index);
        EvaluationContext context{evaluation_time_, *this, node_index};
        const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
        stops.functions[i](node, storage_, &context);
    }

    clear_future_schedule();
    clear_cycle_callbacks();
}

void RuntimeGraph::stop() noexcept {
    auto scratch = details::EvaluationScratch{*memory_};
    stop(scratch);
}

void RuntimeGraph::stop(details::EvaluationScratch &scratch) noexcept {
    if (!started_ || stopped_) {
        return;
    }

    stopped_ = true;
    const auto scratch_use = ScratchUse{*this, scratch};
    stop_nodes(compiled_graph_->node_count());
}

bool RuntimeGraph::inputs_are_valid(const CompiledNode &node) const noexcept {
    const auto runtime_node = Node{node, storage_};
    for (auto parameter_index = std::size_t{}; parameter_index < runtime_node.input_count(); ++parameter_index) {
        const auto &parameter = node.parameters()[parameter_index];
        if (parameter.mode() == ParameterBindingMode::scalar || parameter.validity() != required) {
            continue;
        }

        if (runtime_node.input_last_updated(parameter_index) <= MIN_DT) {
            return false;
        }
    }

    return true;
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time) {
    auto scratch = details::EvaluationScratch{*memory_};
    evaluate(evaluation_time, scratch);
}

void RuntimeGraph::evaluate(EvaluationTime evaluation_time, details::EvaluationScratch &scratch) {
    if (!started_ || stopped_) {
        throw std::logic_error{"a runtime graph must be running to evaluate"};
    }

    if (evaluation_time <= last_evaluation_time_) {
        throw std::invalid_argument{"evaluation time must increase between outer cycles"};
    }

    const auto scratch_use = ScratchUse{*this, scratch};
    run_cycle_callbacks();
    last_evaluation_time_ = evaluation_time;
    evaluation_time_ = evaluation_time;
    run_input_notifications();
    activate_scheduled(evaluation_time);

    auto scheduling = scheduling_info();
    auto scheduled = ScheduledNodes{scheduling};
    auto stop_on_failure = util::scope_exit{[&]() noexcept {
        stopped_ = true;
        stop_nodes(compiled_graph_->node_count());
    }};

    for (const auto [node_index, evaluation_requested, validity_check_requested] : scheduled) {
        const auto word_index = node_index / CompiledGraph::schedule_word_bits;
        const auto mask = ScheduleWord{1} << (node_index % CompiledGraph::schedule_word_bits);
        const auto &node = compiled_graph_->node(node_index);
        const auto has_output = node.has_output();
        const auto inputs_were_valid = scheduling[word_index].inputs_valid(mask);
        auto inputs_valid = inputs_were_valid;

        if (validity_check_requested || (evaluation_requested && !inputs_valid)) {
            inputs_valid = inputs_are_valid(node);
            scheduling[word_index].inputs_valid(mask, inputs_valid);
        }

        if (inputs_were_valid && !inputs_valid) {
            apply_invalidation(node, node_index);
        }

        if (!inputs_valid) {
            continue;
        }

        if (!evaluation_requested) {
            continue;
        }

        const auto output_was_valid = has_output && Node{node, storage_}.output_last_updated() > MIN_DT;
        if (const auto evaluate = node.evaluation_function(); evaluate != nullptr) {
            EvaluationContext context{evaluation_time, *this, node_index};
            const auto allocation_context = details::ScalarAllocationContext{*memory_, this, node_index};
            evaluate(node, storage_, &context);
        }

        if (has_output) {
            publish_output_changes(node, output_was_valid);
        }
    }
    stop_on_failure.release();
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

GlobalState &EvaluationContext::global_state() const noexcept {
    return runtime_graph_->global_state();
}

EvaluationContext::EvaluationContext(EvaluationTime evaluation_time, RuntimeGraph &runtime_graph,
                                     std::size_t node_index) noexcept
    : evaluation_time_(evaluation_time), runtime_graph_(&runtime_graph), node_index_(node_index),
      scratch_{runtime_graph.scratch_} {}

EvaluationContext::~EvaluationContext() = default;

} // namespace bg
