#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/util/index.hpp>
#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/runtime_bunch.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace bg::details {

enum class time_series_set_change {
    none,
    added,
    removed,
    restored,
    canceled,
};

class time_series_set_storage {
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
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    void reset_change_tracking(EvaluationTime evaluation_time, const ScalarRuntimeType &key_runtime_type,
                               const util::runtime_item_layout &key_layout, std::pmr::memory_resource &memory,
                               Reclaim &&reclaim) {
        if (change_time_ == evaluation_time) {
            return;
        }

        for (const auto position : removed_) {
            reclaim_position(position, key_runtime_type, key_layout, memory, reclaim);
        }
        positions_.trim(memory);

        added_.clear();
        removed_.clear();
        change_time_ = evaluation_time;
    }

    template <typename ConstructItem, typename DestroyItem>
    [[nodiscard]] InsertResult insert(const details::ScalarData &key, const ScalarRuntimeType &key_runtime_type,
                                      const util::runtime_item_layout &key_layout, std::pmr::memory_resource &memory,
                                      ConstructItem &&construct_item, DestroyItem &&destroy_item) {
        if (const auto position = find_position(key, key_runtime_type, key_layout);
            position != util::occupancy_index::npos) {
            if (live_.contains(position)) {
                return {position, time_series_set_change::none};
            }

            live_.set(position, memory);
            removed_.reset(position);
            return {position, time_series_set_change::restored};
        }

        const auto position = positions_.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept { positions_.reset(position); }};
        grow_indices(position + 1, memory);

        keys_.emplace_at_with(position, key_layout, memory,
                              [&](void *destination) { key.copy_construct(destination, key_runtime_type, memory); });
        auto release_key = util::scope_exit{[&]() noexcept {
            keys_.erase_at_with(position, key_layout, memory,
                                [&](void *value) noexcept { key_runtime_type.destroy(value, memory); });
        }};

        std::invoke(construct_item, position);
        auto release_item = util::scope_exit{[&]() noexcept { std::invoke(destroy_item, position); }};

        const auto hash = util::linear_hash_table::hash_code(key.hash());
        lookup_.insert(hash, position, memory);
        auto release_registration = util::scope_exit{[&]() noexcept {
            added_.reset(position);
            live_.reset(position);
            lookup_.erase(hash, position, memory);
        }};
        live_.set(position, memory);
        added_.set(position, memory);

        release_registration.release();
        release_item.release();
        release_key.release();
        release_position.release();
        return {position, time_series_set_change::added};
    }

    template <typename Reclaim>
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    [[nodiscard]] EraseResult erase(const details::ScalarData &key, const ScalarRuntimeType &key_runtime_type,
                                    const util::runtime_item_layout &key_layout, std::pmr::memory_resource &memory,
                                    Reclaim &&reclaim) {
        const auto position = find_position(key, key_runtime_type, key_layout);
        if (position == util::occupancy_index::npos || !live_.contains(position)) {
            return {util::occupancy_index::npos, time_series_set_change::none};
        }

        live_.reset(position);
        if (added_.reset(position)) {
            reclaim_position(position, key_runtime_type, key_layout, memory, reclaim);
            positions_.trim(memory);
            return {position, time_series_set_change::canceled};
        }

        removed_.set(position, memory);
        return {position, time_series_set_change::removed};
    }

    template <typename Reclaim>
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    void clear(const ScalarRuntimeType &key_runtime_type, const util::runtime_item_layout &key_layout,
               std::pmr::memory_resource &memory, Reclaim &&reclaim) {
        for (const auto position : live_) {
            live_.reset(position);
            if (added_.reset(position)) {
                reclaim_position(position, key_runtime_type, key_layout, memory, reclaim);
            } else {
                removed_.set(position, memory);
            }
        }
        positions_.trim(memory);
    }

    template <typename ReleaseItem>
        requires std::is_nothrow_invocable_v<ReleaseItem &, Position>
    void release(const ScalarRuntimeType &key_runtime_type, const util::runtime_item_layout &key_layout,
                 std::pmr::memory_resource &memory, ReleaseItem &&release_item) noexcept {
        for (const auto position : positions_) {
            std::invoke(release_item, position);
        }
        keys_.release_with(positions_, key_layout, memory,
                           [&](void *value) noexcept { key_runtime_type.destroy(value, memory); });
        lookup_.release(memory);
        positions_.release(memory);
        live_.release(memory);
        added_.release(memory);
        removed_.release(memory);
    }

    [[nodiscard]] Position find_position(const details::ScalarData &key, const ScalarRuntimeType &key_runtime_type,
                                         const util::runtime_item_layout &key_layout) const {
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto position = lookup_.find(
            hash, [&](Position candidate) { return key_runtime_type.equals(keys_.get(candidate, key_layout), key); });
        return position.value_or(util::occupancy_index::npos);
    }

    [[nodiscard]] bool contains(const details::ScalarData &key, const ScalarRuntimeType &key_runtime_type,
                                const util::runtime_item_layout &key_layout) const {
        const auto position = find_position(key, key_runtime_type, key_layout);
        return position != util::occupancy_index::npos && live_.contains(position);
    }

    [[nodiscard]] const void *key(Position position, const util::runtime_item_layout &key_layout) const noexcept {
        return keys_.get(position, key_layout);
    }
    [[nodiscard]] std::size_t size() const noexcept { return live_.count(); }
    [[nodiscard]] bool changed() const noexcept { return added_.any() || removed_.any(); }
    [[nodiscard]] EvaluationTime change_time() const noexcept { return change_time_; }
    [[nodiscard]] const util::index<> &live() const noexcept { return live_; }
    [[nodiscard]] const util::index<> &added() const noexcept { return added_; }
    [[nodiscard]] const util::index<> &removed() const noexcept { return removed_; }

  private:
    void grow_indices(Position size, std::pmr::memory_resource &memory) {
        live_.grow(size, memory);
        added_.grow(size, memory);
        removed_.grow(size, memory);
    }

    template <typename Reclaim>
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    void reclaim_position(Position position, const ScalarRuntimeType &key_runtime_type,
                          const util::runtime_item_layout &key_layout, std::pmr::memory_resource &memory,
                          Reclaim &&reclaim) noexcept {
        auto *key = keys_.get(position, key_layout);
        const auto hash = util::linear_hash_table::hash_code(key_runtime_type.hash(key));
        lookup_.erase(hash, position, memory);
        std::invoke(reclaim, position);
        keys_.erase_at_with(position, key_layout, memory,
                            [&](void *value) noexcept { key_runtime_type.destroy(value, memory); });
        positions_.reset(position);
    }

    util::occupancy_index positions_;
    util::runtime_sparse_bunch keys_;
    util::linear_hash_table lookup_;
    util::index<> live_;
    util::index<> added_;
    util::index<> removed_;
    EvaluationTime change_time_{MIN_DT};
};

static_assert(std::is_trivially_copyable_v<time_series_set_storage>);

} // namespace bg::details
