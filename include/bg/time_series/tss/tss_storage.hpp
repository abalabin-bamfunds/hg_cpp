#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/scalar/view.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/util/index.hpp>
#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/runtime_bunch.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <utility>

namespace bg::details {

enum class time_series_set_change {
    none,
    added,
    removed,
    restored,
    canceled,
    revalidated,
};

template <typename K> class time_series_set_storage {
    using Position = std::uint32_t;

  public:
    struct InsertResult {
        Position position;
        time_series_set_change change;
    };

    struct EraseResult {
        Position position;
        time_series_set_change change;
    };

    template <typename Reclaim>
    void reset_change_tracking(EvaluationTime evaluation_time, EvaluationTime previous_last_updated,
                               std::pmr::memory_resource &memory, Reclaim &&reclaim) {
        if (change_time_ == evaluation_time) {
            return;
        }

        for (const auto position : retired_) {
            auto *key = keys_.get(position, key_layout());
            const auto hash = util::linear_hash_table::hash_code(key_representation().hash(key));
            static_cast<void>(lookup_.erase(hash, position, memory));
            std::invoke(reclaim, position);
            keys_.erase_at_with(position, key_layout(), memory,
                                [&](void *value) noexcept { key_representation().destroy(value, memory); });
            static_cast<void>(positions_.reset(position));
        }
        positions_.trim(memory);

        added_.clear();
        removed_.clear();
        created_.clear();
        retired_.clear();
        change_time_ = evaluation_time;
        previous_last_updated_ = previous_last_updated;
        revalidated_ = previous_last_updated <= MIN_DT;
    }

    template <typename ConstructItem, typename DestroyItem>
    [[nodiscard]] InsertResult insert(const ScalarValueInputView &key, std::pmr::memory_resource &memory,
                                      ConstructItem &&construct_item, DestroyItem &&destroy_item) {
        if (const auto position = find_position(key); position != util::occupancy_index::npos) {
            if (live_.contains(position)) {
                return {position, revalidated_ ? time_series_set_change::revalidated : time_series_set_change::none};
            }

            static_cast<void>(retired_.reset(position));
            static_cast<void>(live_.set(position, memory));
            if (created_.contains(position)) {
                static_cast<void>(added_.set(position, memory));
                return {position, time_series_set_change::added};
            }

            static_cast<void>(removed_.reset(position));
            return {position, time_series_set_change::restored};
        }

        const auto position = positions_.acquire(memory);
        try {
            grow_indices(position + 1, memory);
        } catch (...) {
            static_cast<void>(positions_.reset(position));
            throw;
        }
        auto key_constructed = false;
        auto item_constructed = false;

        try {
            keys_.emplace_at_with(position, key_layout(), memory, [&](void *destination) {
                key.copy_construct(destination, key_representation(), memory);
            });
            key_constructed = true;
            std::invoke(construct_item, position);
            item_constructed = true;
            lookup_.insert(util::linear_hash_table::hash_code(key.hash()), position, memory);
        } catch (...) {
            if (item_constructed) {
                std::invoke(destroy_item, position);
            }
            if (key_constructed) {
                keys_.erase_at_with(position, key_layout(), memory,
                                    [&](void *value) noexcept { key_representation().destroy(value, memory); });
            }
            static_cast<void>(positions_.reset(position));
            throw;
        }

        static_cast<void>(live_.set(position, memory));
        static_cast<void>(created_.set(position, memory));
        static_cast<void>(added_.set(position, memory));
        return {position, time_series_set_change::added};
    }

    [[nodiscard]] EraseResult erase(const ScalarValueInputView &key, std::pmr::memory_resource &memory) {
        const auto position = find_position(key);
        if (position == util::occupancy_index::npos || !live_.contains(position)) {
            return {util::occupancy_index::npos, time_series_set_change::none};
        }

        static_cast<void>(live_.reset(position));
        static_cast<void>(retired_.set(position, memory));
        if (created_.contains(position)) {
            static_cast<void>(added_.reset(position));
            return {position, time_series_set_change::canceled};
        }

        static_cast<void>(removed_.set(position, memory));
        return {position, time_series_set_change::removed};
    }

    template <typename ReleaseItem>
    void release(std::pmr::memory_resource &memory, ReleaseItem &&release_item) noexcept {
        for (const auto position : positions_) {
            std::invoke(release_item, position);
        }
        keys_.release_with(positions_, key_layout(), memory,
                           [&](void *value) noexcept { key_representation().destroy(value, memory); });
        lookup_.release(memory);
        positions_.release(memory);
        live_.release(memory);
        added_.release(memory);
        removed_.release(memory);
        created_.release(memory);
        retired_.release(memory);
    }

    [[nodiscard]] Position find_position(const ScalarValueInputView &key) const {
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto position = lookup_.find(hash, [&](Position candidate) {
            return key_representation().equals(keys_.get(candidate, key_layout()), key);
        });
        return position.value_or(util::occupancy_index::npos);
    }

    [[nodiscard]] bool contains(const ScalarValueInputView &key) const {
        const auto position = find_position(key);
        return position != util::occupancy_index::npos && live_.contains(position);
    }

    [[nodiscard]] const void *key(Position position) const noexcept { return keys_.get(position, key_layout()); }
    [[nodiscard]] std::size_t size() const noexcept { return live_.count(); }
    [[nodiscard]] bool changed() const noexcept {
        return (revalidated_ && live_.any()) || added_.any() || removed_.any();
    }
    [[nodiscard]] bool revalidated() const noexcept { return revalidated_; }
    [[nodiscard]] EvaluationTime change_time() const noexcept { return change_time_; }
    [[nodiscard]] EvaluationTime previous_last_updated() const noexcept { return previous_last_updated_; }
    [[nodiscard]] const util::index<> &live() const noexcept { return live_; }
    [[nodiscard]] const util::index<> &added() const noexcept { return added_; }
    [[nodiscard]] const util::index<> &removed() const noexcept { return removed_; }

  private:
    [[nodiscard]] static const ScalarStorageRepresentation &key_representation() noexcept {
        return cpp_scalar_storage_representation<K>();
    }

    [[nodiscard]] static const util::runtime_item_layout &key_layout() noexcept {
        static const auto value =
            util::runtime_item_layout{key_representation().size(), key_representation().alignment()};
        return value;
    }

    void grow_indices(Position size, std::pmr::memory_resource &memory) {
        live_.grow(size, memory);
        added_.grow(size, memory);
        removed_.grow(size, memory);
        created_.grow(size, memory);
        retired_.grow(size, memory);
    }

    util::occupancy_index positions_;
    util::runtime_sparse_bunch keys_;
    util::linear_hash_table lookup_;
    util::index<> live_;
    util::index<> added_;
    util::index<> removed_;
    util::index<> created_;
    util::index<> retired_;
    EvaluationTime change_time_{MIN_DT};
    EvaluationTime previous_last_updated_{MIN_DT};
    bool revalidated_{};
};

template <typename K, auto... Properties> struct time_series_value_storage<TSS<K, Properties...>> {
    using type = time_series_set_storage<K>;
};

} // namespace bg::details
