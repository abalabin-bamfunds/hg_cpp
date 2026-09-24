#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/graph/nested_graph.hpp>
#include <bg/graph/runtime_graph_owner.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/index.hpp>

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

// Owns the inner runtime graph, lifecycle state, and scheduling state for every map entry.
class MapEntries {
  public:
    struct Entry {
        static constexpr auto not_in_schedule_heap = std::numeric_limits<std::size_t>::max();

        NestedGraph inner_graph;
        EvaluationTime next_scheduled_time{MIN_DT};
        std::size_t schedule_heap_index{not_in_schedule_heap};
        std::uint32_t entry_index{};
        std::uint32_t presence_count : 30 {};
        std::uint32_t presence_change_queued : 1 {};
        std::uint32_t removal_queued : 1 {};
    };

    using Table = util::hash_dictionary<ScalarValue<>, Entry, ScalarHash, ScalarEqual>;
    using size_type = Table::size_type;

    explicit MapEntries(allocator memory);
    ~MapEntries();

    MapEntries(const MapEntries &) = delete;
    MapEntries &operator=(const MapEntries &) = delete;

    allocator allocation() const noexcept { return memory_; }
    Table &table() noexcept { return table_; }
    const Table &table() const noexcept { return table_; }

    template <typename Key> Entry &require(const Key &key) {
        if (auto *entry = table_.find(key)) {
            return *entry;
        }

        auto stored_key = ScalarValue<>{Scalar<>::from(key), memory_};
        auto [entry, inserted] = table_.try_emplace(std::move(stored_key), memory_.memory_resource());
        (void)inserted;
        entry->entry_index = table_.find_position(key);
        // Owner scheduling callbacks cannot allocate. Reserve the ready bit before the graph can invoke them.
        ready_.grow(entry->entry_index + 1, memory_.memory_resource());
        return *entry;
    }

    template <typename Key> Entry *find(const Key &key) { return table_.find(key); }
    template <typename Key> const Entry *find(const Key &key) const { return table_.find(key); }

    Scalar<> key(const Entry &entry) const noexcept { return table_.key_at(entry.entry_index).view(); }

    void start(Entry &entry, const CompiledNestedGraph &compiled, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants = {});
    void start(Entry &entry, const CompiledNestedGraph &compiled, EvaluationContext &context,
               std::span<const NestedGraphInput> inputs, std::span<const NestedGraphConstantInput> constants,
               RuntimeGraphOwner owner);
    void stop(Entry &entry, EvaluationContext &context);
    void stop_all(EvaluationContext &context) noexcept;
    void destroy_all(EvaluationContext &context) noexcept;

    void mark_ready(Entry &entry) noexcept;
    void mark_ready(size_type entry_index) noexcept;
    util::index<> &ready() noexcept { return ready_; }

    EvaluationResult evaluate(Entry &entry, EvaluationContext &context, RuntimeGraphEvaluation &evaluation);
    void update_input(Entry &entry, const CompiledNestedGraph &compiled, std::size_t input_index,
                      NestedGraphInput input);

    void activate_due(EvaluationContext &context);
    void clear_schedule_tracking(EvaluationContext &context) noexcept;
    void timed_schedule_changed(RuntimeGraphOwnerContext &context, size_type entry_index,
                                std::optional<EvaluationTime> time);

  private:
    static void inner_graph_scheduled(void *receiver, RuntimeGraphOwnerCallbacks::token_type token) noexcept;
    static void inner_graph_timed_schedule_changed(RuntimeGraphOwnerContext &context, void *receiver,
                                                   RuntimeGraphOwnerCallbacks::token_type token,
                                                   std::optional<EvaluationTime> time);
    static void remove_stopped_entries_callback(void *receiver) noexcept;

    void queue_removal(Entry &entry, EvaluationContext &context);
    void remove_stopped_entries() noexcept;
    void set_next_scheduled_time(RuntimeGraphOwnerContext &context, Entry &entry, std::optional<EvaluationTime> time);
    void remove_from_schedule_heap(Entry &entry) noexcept;
    void move_up_schedule_heap(std::size_t index) noexcept;
    void move_down_schedule_heap(std::size_t index) noexcept;
    void swap_schedule_heap_items(std::size_t left, std::size_t right) noexcept;
    static bool scheduled_before(const Entry *left, const Entry *right) noexcept;

    static const RuntimeGraphOwnerCallbacks owner_callbacks;

    allocator memory_;
    Table table_;
    std::pmr::vector<Entry *> schedule_heap_;
    util::index<> ready_;
    std::pmr::vector<size_type> entries_pending_removal_;
    ScheduleHandle outer_schedule_{};
    std::optional<EvaluationTime> outer_scheduled_time_;
    bool removal_callback_pending_{};
};

static_assert(sizeof(MapEntries::Entry) <= 5 * sizeof(void *));

} // namespace details
} // namespace bg
