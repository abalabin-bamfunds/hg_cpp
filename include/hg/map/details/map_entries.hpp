#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/graph/nested_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/runtime_graph_scheduling.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/index.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace bg {

class EvaluationContext;

namespace details {

using branch_id_t = std::uint32_t;

// Owns the inner runtime graph, lifecycle state, and scheduling state for every map entry.
class MapEntries {
  public:
    struct Entry {
        NestedGraph inner_graph;
        branch_id_t branch_id{};
        std::uint32_t presence_count : 30 {};
        std::uint32_t presence_change_queued : 1 {};
        std::uint32_t removal_queued : 1 {};
    };

    using Table = util::hash_dictionary<ScalarValue<>, Entry, ScalarHash, ScalarEqual>;
    static_assert(std::same_as<Table::size_type, branch_id_t>);

    explicit MapEntries(allocator memory);
    ~MapEntries();

    MapEntries(const MapEntries &) = delete;
    MapEntries &operator=(const MapEntries &) = delete;

    allocator allocation() const noexcept { return memory_; }
    Table &table() noexcept { return table_; }
    const Table &table() const noexcept { return table_; }

    void bind_evaluator(EvaluationContext &context);
    void bind_evaluator(EvaluationContext &context, RuntimeGraphSchedulingHandler handler);

    template <typename Key> Entry &require(const Key &key) {
        if (auto *entry = table_.find(key)) {
            return *entry;
        }

        auto stored_key = ScalarValue<>{Scalar<>::from(key), memory_};
        auto [entry, inserted] = table_.try_emplace(std::move(stored_key), memory_.memory_resource());
        (void)inserted;
        entry->branch_id = table_.find_position(key);
        // Scheduling callbacks cannot allocate. Reserve the ready bit before the graph can invoke them.
        ready_.grow(entry->branch_id + 1, memory_.memory_resource());
        return *entry;
    }

    template <typename Key> Entry *find(const Key &key) { return table_.find(key); }
    template <typename Key> const Entry *find(const Key &key) const { return table_.find(key); }

    Scalar<> key(const Entry &entry) const noexcept { return table_.key_at(entry.branch_id).view(); }

    void start(Entry &entry, const CompiledNestedGraph &compiled, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants = {});
    void stop(Entry &entry, EvaluationContext &context);
    void stop_all(EvaluationContext &context) noexcept;
    void destroy_all(EvaluationContext &context) noexcept;

    void mark_ready(Entry &entry) noexcept;
    void mark_ready(branch_id_t branch_id) noexcept;
    util::index<> &ready() noexcept { return ready_; }

  private:
    static void inner_graph_scheduled(void *receiver, RuntimeGraphSchedulingHandler::token_type token) noexcept;
    static void remove_stopped_entries_callback(void *receiver, EvaluationContext &) noexcept;

    void queue_removal(Entry &entry, EvaluationContext &context);
    void remove_stopped_entries() noexcept;
    allocator memory_;
    RuntimeGraphEvaluator evaluator_;
    Table table_;
    util::index<> ready_;
    std::pmr::vector<branch_id_t> branches_pending_removal_;
    bool removal_callback_pending_{};
};

static_assert(sizeof(MapEntries::Entry) <= 5 * sizeof(void *));

} // namespace details
} // namespace bg
