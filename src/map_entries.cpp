#include <bg/map/details/map_entries.hpp>

#include <bg/evaluation_context.hpp>

#include <algorithm>
#include <utility>

namespace bg::details {

MapEntries::MapEntries(allocator memory)
    : memory_{memory}, schedule_heap_{&memory.memory_resource()}, branches_pending_removal_{&memory.memory_resource()} {
}

MapEntries::~MapEntries() {
    ready_.release(memory_.memory_resource());
    table_.release(memory_.memory_resource());
}

void MapEntries::start(Entry &entry, const CompiledNestedGraph &compiled, EvaluationContext &context,
                       std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants) {
    const auto owner = RuntimeGraphOwner{
        .receiver = this,
        .token = entry.branch_id,
        .callbacks = &owner_callbacks,
    };
    start(entry, compiled, context, inputs, constants, owner);
}

void MapEntries::start(Entry &entry, const CompiledNestedGraph &compiled, EvaluationContext &context,
                       std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants,
                       RuntimeGraphOwner owner) {
    entry.inner_graph.start(compiled, context, inputs, constants, owner);
    mark_ready(entry);
}

void MapEntries::stop(Entry &entry, EvaluationContext &context) {
    if (!entry.inner_graph.running()) {
        return;
    }

    queue_removal(entry, context);
    entry.inner_graph.stop(context);
    ready_.reset(entry.branch_id);
}

void MapEntries::stop_all(EvaluationContext &context) noexcept {
    clear_schedule_tracking(context);

    for (auto [key, entry] : table_) {
        if (!entry.inner_graph.running()) {
            continue;
        }
        entry.inner_graph.stop(context);
        ready_.reset(entry.branch_id);
    }
}

void MapEntries::destroy_all(EvaluationContext &context) noexcept {
    clear_schedule_tracking(context);
    for (auto [key, entry] : table_) {
        entry.inner_graph.stop_and_destroy(context);
    }
}

void MapEntries::mark_ready(Entry &entry) noexcept {
    if (entry.inner_graph.running()) {
        ready_.set(entry.branch_id, memory_.memory_resource());
    }
}

void MapEntries::mark_ready(branch_id_t branch_id) noexcept {
    mark_ready(table_.value_at(branch_id));
}

void MapEntries::inner_graph_scheduled(void *receiver, RuntimeGraphOwnerCallbacks::token_type token) noexcept {
    static_cast<MapEntries *>(receiver)->mark_ready(static_cast<branch_id_t>(token));
}

void MapEntries::inner_graph_timed_schedule_changed(RuntimeGraphOwnerContext &context, void *receiver,
                                                    RuntimeGraphOwnerCallbacks::token_type token,
                                                    std::optional<EvaluationTime> time) {
    auto &entries = *static_cast<MapEntries *>(receiver);
    entries.set_next_scheduled_time(context, entries.table_.value_at(static_cast<branch_id_t>(token)), time);
}

void MapEntries::timed_schedule_changed(RuntimeGraphOwnerContext &context, branch_id_t branch_id,
                                        std::optional<EvaluationTime> time) {
    set_next_scheduled_time(context, table_.value_at(branch_id), time);
}

const RuntimeGraphOwnerCallbacks MapEntries::owner_callbacks{
    .inner_scheduled = inner_graph_scheduled,
    .timed_schedule_changed = inner_graph_timed_schedule_changed,
};

void MapEntries::queue_removal(Entry &entry, EvaluationContext &context) {
    if (!removal_callback_pending_) {
        context.add_cycle_callback(this, remove_stopped_entries_callback);
        removal_callback_pending_ = true;
    }
    if (entry.removal_queued) {
        return;
    }

    entry.removal_queued = true;
    branches_pending_removal_.push_back(entry.branch_id);
}

void MapEntries::remove_stopped_entries_callback(void *receiver) noexcept {
    static_cast<MapEntries *>(receiver)->remove_stopped_entries();
}

void MapEntries::remove_stopped_entries() noexcept {
    removal_callback_pending_ = false;
    for (const auto branch_id : branches_pending_removal_) {
        auto &entry = table_.value_at(branch_id);
        entry.removal_queued = false;
        if (entry.inner_graph.running() || entry.presence_count != 0) {
            continue;
        }
        table_.erase(table_.key_at(branch_id).view(), memory_.memory_resource());
    }
    branches_pending_removal_.clear();
}

void MapEntries::clear_schedule_tracking(EvaluationContext &context) noexcept {
    if (outer_scheduled_time_) {
        context.cancel(outer_schedule_);
        outer_schedule_ = {};
        outer_scheduled_time_.reset();
    }
    for (auto *entry : schedule_heap_) {
        entry->schedule_heap_index = Entry::not_in_schedule_heap;
        entry->next_scheduled_time = MIN_DT;
    }
    schedule_heap_.clear();
}

bool MapEntries::scheduled_before(const Entry *left, const Entry *right) noexcept {
    return left->next_scheduled_time < right->next_scheduled_time;
}

void MapEntries::swap_schedule_heap_items(std::size_t left, std::size_t right) noexcept {
    std::swap(schedule_heap_[left], schedule_heap_[right]);
    schedule_heap_[left]->schedule_heap_index = left;
    schedule_heap_[right]->schedule_heap_index = right;
}

void MapEntries::move_up_schedule_heap(std::size_t index) noexcept {
    while (index != 0) {
        const auto parent = (index - 1) / 2;
        if (!scheduled_before(schedule_heap_[index], schedule_heap_[parent])) {
            break;
        }
        swap_schedule_heap_items(index, parent);
        index = parent;
    }
}

void MapEntries::move_down_schedule_heap(std::size_t index) noexcept {
    while (true) {
        const auto left = index * 2 + 1;
        if (left >= schedule_heap_.size()) {
            return;
        }
        const auto right = left + 1;
        const auto first =
            right < schedule_heap_.size() && scheduled_before(schedule_heap_[right], schedule_heap_[left]) ? right
                                                                                                           : left;
        if (!scheduled_before(schedule_heap_[first], schedule_heap_[index])) {
            return;
        }
        swap_schedule_heap_items(index, first);
        index = first;
    }
}

void MapEntries::remove_from_schedule_heap(Entry &entry) noexcept {
    if (entry.schedule_heap_index == Entry::not_in_schedule_heap) {
        return;
    }
    const auto index = entry.schedule_heap_index;
    entry.schedule_heap_index = Entry::not_in_schedule_heap;
    if (index == schedule_heap_.size() - 1) {
        schedule_heap_.pop_back();
        return;
    }

    schedule_heap_[index] = schedule_heap_.back();
    schedule_heap_.pop_back();
    schedule_heap_[index]->schedule_heap_index = index;
    move_up_schedule_heap(index);
    move_down_schedule_heap(schedule_heap_[index]->schedule_heap_index);
}

void MapEntries::set_next_scheduled_time(RuntimeGraphOwnerContext &context, Entry &entry,
                                         std::optional<EvaluationTime> time) {
    const auto previous_first = schedule_heap_.empty()
                                    ? std::optional<EvaluationTime>{}
                                    : std::optional<EvaluationTime>{schedule_heap_.front()->next_scheduled_time};
    remove_from_schedule_heap(entry);
    entry.next_scheduled_time = time.value_or(MIN_DT);
    if (time) {
        entry.schedule_heap_index = schedule_heap_.size();
        schedule_heap_.push_back(&entry);
        move_up_schedule_heap(entry.schedule_heap_index);
    }

    const auto next_first = schedule_heap_.empty()
                                ? std::optional<EvaluationTime>{}
                                : std::optional<EvaluationTime>{schedule_heap_.front()->next_scheduled_time};
    if (next_first == previous_first) {
        return;
    }
    if (outer_scheduled_time_) {
        context.cancel(outer_schedule_);
        outer_schedule_ = {};
    }
    outer_scheduled_time_ = next_first;
    if (next_first) {
        outer_schedule_ = context.schedule_at(*next_first);
    }
}

void MapEntries::activate_due(EvaluationContext &context) {
    const auto evaluation_time = context.evaluation_time();
    if (outer_scheduled_time_ && *outer_scheduled_time_ <= evaluation_time) {
        outer_schedule_ = {};
        outer_scheduled_time_.reset();
    }
    while (!schedule_heap_.empty() && schedule_heap_.front()->next_scheduled_time <= evaluation_time) {
        auto *entry = schedule_heap_.front();
        remove_from_schedule_heap(*entry);
        entry->next_scheduled_time = MIN_DT;
        mark_ready(*entry);
    }
    if (!outer_scheduled_time_ && !schedule_heap_.empty()) {
        outer_scheduled_time_ = schedule_heap_.front()->next_scheduled_time;
        outer_schedule_ = context.schedule_at(*outer_scheduled_time_);
    }
}

} // namespace bg::details
