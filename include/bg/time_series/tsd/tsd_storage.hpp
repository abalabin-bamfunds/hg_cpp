#pragma once

#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/util/index.hpp>
#include <bg/util/runtime_bunch.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <type_traits>

namespace bg::details {

[[nodiscard]] inline std::size_t contained_time_series_value_offset(
    const TimeSeriesStorageRepresentation &representation) noexcept {
    const auto alignment = representation.value_alignment();
    const auto remainder = sizeof(EvaluationTime) % alignment;
    return remainder == 0 ? sizeof(EvaluationTime) : sizeof(EvaluationTime) + alignment - remainder;
}

[[nodiscard]] inline util::runtime_item_layout contained_time_series_layout(
    const TimeSeriesStorageRepresentation &representation) noexcept {
    const auto alignment = std::max(alignof(EvaluationTime), representation.value_alignment());
    const auto value_offset = contained_time_series_value_offset(representation);
    const auto size = value_offset + representation.value_size();
    const auto remainder = size % alignment;
    return {remainder == 0 ? size : size + alignment - remainder, alignment};
}

struct time_series_set_slot {
    EvaluationTime last_updated{MIN_DT};
    time_series_set_storage value;
};

class time_series_dict_storage {
    using Position = std::uint32_t;

  public:
    [[nodiscard]] static consteval std::size_t key_last_updated_offset() noexcept {
        return offsetof(time_series_dict_storage, keys) + offsetof(time_series_set_slot, last_updated);
    }

    [[nodiscard]] static consteval std::size_t key_value_offset() noexcept {
        return offsetof(time_series_dict_storage, keys) + offsetof(time_series_set_slot, value);
    }

    void reset_change_tracking(EvaluationTime evaluation_time, const ScalarStorageRepresentation &key_representation,
                               const util::runtime_item_layout &key_layout,
                               const TimeSeriesStorageRepresentation &item_representation,
                               const util::runtime_item_layout &item_layout, std::size_t item_value_offset,
                               std::pmr::memory_resource &memory) {
        if (change_time_ == evaluation_time) {
            return;
        }

        keys.value.reset_change_tracking(evaluation_time, key_representation, key_layout, memory,
                                         [&](Position position) noexcept {
                                             destroy_item(position, item_representation, item_layout,
                                                          item_value_offset, memory);
                                         });
        modified_.clear();
        change_time_ = evaluation_time;
    }

    [[nodiscard]] time_series_set_storage::InsertResult
    item(const ScalarValueInputView &key, const ScalarStorageRepresentation &key_representation,
         const util::runtime_item_layout &key_layout, const TimeSeriesStorageRepresentation &item_representation,
         const util::runtime_item_layout &item_layout, std::size_t item_value_offset,
         std::pmr::memory_resource &memory) {
        return keys.value.insert(
            key, key_representation, key_layout, memory,
            [&](Position position) {
                modified_.grow(position + 1, memory);
                items_.emplace_at_with(position, item_layout, memory, [&](void *item) {
                    auto *bytes = static_cast<std::byte *>(item);
                    std::construct_at(reinterpret_cast<EvaluationTime *>(bytes), MIN_DT);
                    auto rollback = util::scope_exit{[&]() noexcept {
                        std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes));
                    }};
                    item_representation.construct_runtime_value(bytes + item_value_offset, memory);
                    rollback.release();
                });
            },
            [&](Position position) noexcept {
                destroy_item(position, item_representation, item_layout, item_value_offset, memory);
            });
    }

    [[nodiscard]] time_series_set_storage::EraseResult
    erase(const ScalarValueInputView &key, const ScalarStorageRepresentation &key_representation,
          const util::runtime_item_layout &key_layout, const TimeSeriesStorageRepresentation &item_representation,
          const util::runtime_item_layout &item_layout, std::size_t item_value_offset,
          std::pmr::memory_resource &memory) {
        const auto result = keys.value.erase(
            key, key_representation, key_layout, memory, [&](Position position) noexcept {
                destroy_item(position, item_representation, item_layout, item_value_offset, memory);
            });
        if (result.position != util::occupancy_index::npos) {
            modified_.reset(result.position);
        }
        return result;
    }

    void clear(const ScalarStorageRepresentation &key_representation, const util::runtime_item_layout &key_layout,
               const TimeSeriesStorageRepresentation &item_representation,
               const util::runtime_item_layout &item_layout, std::size_t item_value_offset,
               std::pmr::memory_resource &memory) {
        keys.value.clear(key_representation, key_layout, memory, [&](Position position) noexcept {
            modified_.reset(position);
            destroy_item(position, item_representation, item_layout, item_value_offset, memory);
        });
        for (const auto position : keys.value.removed()) {
            modified_.reset(position);
        }
    }

    [[nodiscard]] bool mark_item_modified(Position position, EvaluationTime evaluation_time,
                                          std::pmr::memory_resource &memory) {
        if (change_time_ != evaluation_time || !keys.value.live().contains(position)) {
            return false;
        }
        return modified_.set(position, memory);
    }

    void mark_item_resurrected(Position position, EvaluationTime evaluation_time, std::pmr::memory_resource &memory) {
        if (change_time_ == evaluation_time && keys.value.live().contains(position)) {
            modified_.set(position, memory);
        }
    }

    [[nodiscard]] EvaluationTime change_time() const noexcept { return change_time_; }
    [[nodiscard]] time_series_set_slot &key_storage() noexcept { return keys; }
    [[nodiscard]] const time_series_set_slot &key_storage() const noexcept { return keys; }
    [[nodiscard]] const util::index<> &modified() const noexcept { return modified_; }

    [[nodiscard]] EvaluationTime &item_last_updated(Position position,
                                                    const util::runtime_item_layout &layout) noexcept {
        return *static_cast<EvaluationTime *>(items_.get(position, layout));
    }

    [[nodiscard]] const EvaluationTime &item_last_updated(Position position,
                                                          const util::runtime_item_layout &layout) const noexcept {
        return *static_cast<const EvaluationTime *>(items_.get(position, layout));
    }

    [[nodiscard]] void *item_value(Position position, const util::runtime_item_layout &layout,
                                   std::size_t value_offset) noexcept {
        return static_cast<std::byte *>(items_.get(position, layout)) + value_offset;
    }

    [[nodiscard]] const void *item_value(Position position, const util::runtime_item_layout &layout,
                                         std::size_t value_offset) const noexcept {
        return static_cast<const std::byte *>(items_.get(position, layout)) + value_offset;
    }

    void release(const ScalarStorageRepresentation &key_representation, const util::runtime_item_layout &key_layout,
                 const TimeSeriesStorageRepresentation &item_representation,
                 const util::runtime_item_layout &item_layout, std::size_t item_value_offset,
                 std::pmr::memory_resource &memory) noexcept {
        keys.value.release(key_representation, key_layout, memory, [&](Position position) noexcept {
            destroy_item(position, item_representation, item_layout, item_value_offset, memory);
        });
        items_.release(item_layout, memory);
        modified_.release(memory);
    }

  private:
    void destroy_item(Position position, const TimeSeriesStorageRepresentation &item_representation,
                      const util::runtime_item_layout &item_layout, std::size_t item_value_offset,
                      std::pmr::memory_resource &memory) noexcept {
        items_.erase_at_with(position, item_layout, memory, [&](void *item) noexcept {
            auto *bytes = static_cast<std::byte *>(item);
            item_representation.destroy_value(bytes + item_value_offset, memory);
            std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes));
        });
    }

    time_series_set_slot keys;
    util::runtime_sparse_bunch items_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
};

static_assert(std::is_trivially_copyable_v<time_series_dict_storage>);

} // namespace bg::details
