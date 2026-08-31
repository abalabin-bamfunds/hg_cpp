#pragma once

#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/util/index.hpp>
#include <bg/util/runtime_bunch.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace bg::details {

template <typename K, typename Item> class time_series_dict_storage {
    using Position = std::uint32_t;
    using Contained = contained_time_series_storage<Item>;

  public:
    using KeyStorage = contained_time_series_storage<TSS<K>>;

    [[nodiscard]] static consteval std::size_t key_last_updated_offset() noexcept {
        return offsetof(time_series_dict_storage, keys) + offsetof(KeyStorage, last_updated);
    }

    [[nodiscard]] static consteval std::size_t key_value_offset() noexcept {
        return offsetof(time_series_dict_storage, keys) + offsetof(KeyStorage, value);
    }

    void reset_change_tracking(EvaluationTime evaluation_time, EvaluationTime previous_last_updated,
                               const TimeSeriesStorageRepresentation &item_representation,
                               std::pmr::memory_resource &memory) {
        if (change_time_ == evaluation_time) {
            return;
        }

        keys.value.value.reset_change_tracking(evaluation_time, keys.last_updated, memory, [&](Position position) {
            items_.erase_at_with(position, item_layout(), memory, [&](void *item) noexcept {
                auto *contained = static_cast<Contained *>(item);
                item_representation.destroy_value(&contained->value.value, memory);
                std::destroy_at(contained);
            });
        });
        modified_.clear();
        resurrected_.clear();
        change_time_ = evaluation_time;
        previous_last_updated_ = previous_last_updated;
        revalidated_ = previous_last_updated <= MIN_DT;
    }

    [[nodiscard]] typename time_series_set_storage<K>::InsertResult
    item(const ScalarValueInputView &key, const TimeSeriesStorageRepresentation &item_representation,
         std::pmr::memory_resource &memory) {
        return keys.value.value.insert(
            key, memory,
            [&](Position position) {
                modified_.grow(position + 1, memory);
                items_.emplace_at_with(position, item_layout(), memory, [&](void *item) {
                    auto *contained = std::construct_at(static_cast<Contained *>(item));
                    try {
                        item_representation.construct_runtime_value(&contained->value.value, memory);
                    } catch (...) {
                        std::destroy_at(contained);
                        throw;
                    }
                });
            },
            [&](Position position) noexcept {
                items_.erase_at_with(position, item_layout(), memory, [&](void *item) noexcept {
                    auto *contained = static_cast<Contained *>(item);
                    item_representation.destroy_value(&contained->value.value, memory);
                    std::destroy_at(contained);
                });
            });
    }

    [[nodiscard]] typename time_series_set_storage<K>::EraseResult erase(const ScalarValueInputView &key,
                                                                         std::pmr::memory_resource &memory) {
        const auto result = keys.value.value.erase(key, memory);
        if (result.position != util::occupancy_index::npos) {
            static_cast<void>(modified_.reset(result.position));
        }
        return result;
    }

    [[nodiscard]] bool mark_item_modified(Position position, EvaluationTime evaluation_time,
                                          std::pmr::memory_resource &memory) {
        if (change_time_ != evaluation_time || !keys.value.value.live().contains(position)) {
            return false;
        }
        return modified_.set(position, memory);
    }

    void mark_item_resurrected(Position position, EvaluationTime evaluation_time,
                               std::pmr::memory_resource &memory) {
        if (change_time_ != evaluation_time || !keys.value.value.live().contains(position)) {
            return;
        }

        static_cast<void>(resurrected_.set(position, memory));
        static_cast<void>(modified_.set(position, memory));
    }

    void clear_item_modified(Position position) noexcept { static_cast<void>(modified_.reset(position)); }

    [[nodiscard]] bool changed() const noexcept {
        return (revalidated_ && keys.value.value.live().any()) || keys.value.value.changed() || modified_.any();
    }

    [[nodiscard]] bool revalidated() const noexcept { return revalidated_; }
    [[nodiscard]] EvaluationTime change_time() const noexcept { return change_time_; }
    [[nodiscard]] EvaluationTime previous_last_updated() const noexcept { return previous_last_updated_; }
    [[nodiscard]] KeyStorage &key_storage() noexcept { return keys; }
    [[nodiscard]] const KeyStorage &key_storage() const noexcept { return keys; }
    [[nodiscard]] const util::index<> &modified() const noexcept { return modified_; }
    [[nodiscard]] const util::index<> &resurrected() const noexcept { return resurrected_; }

    [[nodiscard]] Contained &contained(Position position) noexcept {
        return *static_cast<Contained *>(items_.get(position, item_layout()));
    }

    [[nodiscard]] const Contained &contained(Position position) const noexcept {
        return *static_cast<const Contained *>(items_.get(position, item_layout()));
    }

    void release(const TimeSeriesStorageRepresentation &item_representation,
                 std::pmr::memory_resource &memory) noexcept {
        keys.value.value.release(memory, [&](Position position) noexcept {
            items_.erase_at_with(position, item_layout(), memory, [&](void *item) noexcept {
                auto *contained = static_cast<Contained *>(item);
                item_representation.destroy_value(&contained->value.value, memory);
                std::destroy_at(contained);
            });
        });
        items_.release(item_layout(), memory);
        modified_.release(memory);
        resurrected_.release(memory);
    }

  private:
    [[nodiscard]] static const util::runtime_item_layout &item_layout() noexcept {
        static const auto value = util::runtime_item_layout{sizeof(Contained), alignof(Contained)};
        return value;
    }

    KeyStorage keys;
    util::runtime_sparse_bunch items_;
    util::index<> modified_;
    util::index<> resurrected_;
    EvaluationTime change_time_{MIN_DT};
    EvaluationTime previous_last_updated_{MIN_DT};
    bool revalidated_{};
};

template <typename K, typename Item, auto... Properties> struct time_series_value_storage<TSD<K, Item, Properties...>> {
    using type = time_series_dict_storage<K, Item>;
};

} // namespace bg::details
