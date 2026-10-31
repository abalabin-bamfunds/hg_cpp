#include <bg/map/details/map_entries.hpp>

#include <bg/evaluation_context.hpp>

#include <algorithm>
#include <utility>

namespace bg::details {

MapEntries::MapEntries(allocator memory)
    : memory_{memory}, branches_pending_removal_{&memory.memory_resource()} {
}

MapEntries::~MapEntries() {
    ready_.release(memory_.memory_resource());
    table_.release(memory_.memory_resource());
}

void MapEntries::bind_evaluator(EvaluationContext &context) {
    bind_evaluator(context, {.receiver = this, .inner_scheduled = inner_graph_scheduled});
}

void MapEntries::bind_evaluator(EvaluationContext &context, RuntimeGraphSchedulingHandler handler) {
    evaluator_.bind_owner(context.runtime_graph(), context.node_index(), handler);
}

void MapEntries::start(Entry &entry, const CompiledNestedGraph &compiled, EvaluationContext &context,
                       std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants) {
    entry.inner_graph.start(compiled, evaluator_, context, inputs, constants, entry.branch_id, key(entry));
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
    for (auto [key, entry] : table_) {
        if (!entry.inner_graph.running()) {
            continue;
        }
        entry.inner_graph.stop(context);
        ready_.reset(entry.branch_id);
    }
}

void MapEntries::destroy_all(EvaluationContext &context) noexcept {
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

void MapEntries::inner_graph_scheduled(void *receiver, RuntimeGraphSchedulingHandler::token_type token) noexcept {
    static_cast<MapEntries *>(receiver)->mark_ready(static_cast<branch_id_t>(token));
}

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

void MapEntries::remove_stopped_entries_callback(void *receiver, EvaluationContext &) noexcept {
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

} // namespace bg::details
