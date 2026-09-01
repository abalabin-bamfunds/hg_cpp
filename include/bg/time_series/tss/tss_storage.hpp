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
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    void reset_change_tracking(EvaluationTime evaluation_time, std::pmr::memory_resource &memory, Reclaim &&reclaim) {
        if (change_time_ == evaluation_time) {
            return;
        }

        for (const auto position : removed_) {
            reclaim_position(position, memory, reclaim);
        }
        positions_.trim(memory);

        added_.clear();
        removed_.clear();
        change_time_ = evaluation_time;
    }

    template <typename ConstructItem, typename DestroyItem>
    [[nodiscard]] InsertResult insert(const ScalarValueInputView &key, std::pmr::memory_resource &memory,
                                      ConstructItem &&construct_item, DestroyItem &&destroy_item) {
        if (const auto position = find_position(key); position != util::occupancy_index::npos) {
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

        keys_.emplace_at_with(position, key_layout(), memory, [&](void *destination) {
            key.copy_construct(destination, key_representation(), memory);
        });
        auto release_key = util::scope_exit{[&]() noexcept {
            keys_.erase_at_with(position, key_layout(), memory,
                                [&](void *value) noexcept { key_representation().destroy(value, memory); });
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
    [[nodiscard]] EraseResult erase(const ScalarValueInputView &key, std::pmr::memory_resource &memory,
                                    Reclaim &&reclaim) {
        const auto position = find_position(key);
        if (position == util::occupancy_index::npos || !live_.contains(position)) {
            return {util::occupancy_index::npos, time_series_set_change::none};
        }

        live_.reset(position);
        if (added_.reset(position)) {
            reclaim_position(position, memory, reclaim);
            positions_.trim(memory);
            return {position, time_series_set_change::canceled};
        }

        removed_.set(position, memory);
        return {position, time_series_set_change::removed};
    }

    template <typename Reclaim>
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    void clear(std::pmr::memory_resource &memory, Reclaim &&reclaim) {
        for (const auto position : live_) {
            live_.reset(position);
            if (added_.reset(position)) {
                reclaim_position(position, memory, reclaim);
            } else {
                removed_.set(position, memory);
            }
        }
        positions_.trim(memory);
    }

    template <typename ReleaseItem>
        requires std::is_nothrow_invocable_v<ReleaseItem &, Position>
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
    [[nodiscard]] bool changed() const noexcept { return added_.any() || removed_.any(); }
    [[nodiscard]] EvaluationTime change_time() const noexcept { return change_time_; }
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
    }

    template <typename Reclaim>
        requires std::is_nothrow_invocable_v<Reclaim &, Position>
    void reclaim_position(Position position, std::pmr::memory_resource &memory, Reclaim &&reclaim) noexcept {
        auto *key = keys_.get(position, key_layout());
        const auto hash = util::linear_hash_table::hash_code(key_representation().hash(key));
        lookup_.erase(hash, position, memory);
        std::invoke(reclaim, position);
        keys_.erase_at_with(position, key_layout(), memory,
                            [&](void *value) noexcept { key_representation().destroy(value, memory); });
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

template <typename K, auto... Properties> struct time_series_value_storage<TSS<K, Properties...>> {
    using type = time_series_set_storage<K>;
};

static_assert(std::is_trivially_copyable_v<time_series_set_storage<int>>);

} // namespace bg::details
