#pragma once

#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace bg::util {

// Stores trivially copyable keys at stable addresses and adds a hash index when linear lookup becomes expensive.
// Erased keys remain constructed so linear lookup can reject most positions before consulting the occupancy index.
// The caller must use one memory resource for all storage changes and release storage before destruction.
template <typename Key, std::uint32_t IndexThreshold, typename Hash = std::hash<Key>, typename Equal = std::equal_to<>>
class adaptive_set {
    static_assert(IndexThreshold != 0);
    static_assert(std::is_trivially_copyable_v<Key> && std::is_copy_assignable_v<Key>);

  public:
    using size_type = std::uint32_t;

    adaptive_set() = default;

    adaptive_set(const adaptive_set &) = delete;
    adaptive_set &operator=(const adaptive_set &) = delete;
    adaptive_set(adaptive_set &&) = delete;
    adaptive_set &operator=(adaptive_set &&) = delete;

    template <typename Source>
        requires std::constructible_from<Key, Source> && std::invocable<const Hash &, const Source &> &&
                 std::predicate<const Equal &, const Key &, const Source &>
    std::pair<const Key *, bool> insert(Source &&key, std::pmr::memory_resource &memory) {
        if (const auto position = find_position(key); position != occupancy_index::npos) {
            return {&keys_[position], false};
        }

        const auto previous_extent = positions_.extent();
        const auto position = positions_.acquire(memory);
        auto keep_extent = position != previous_extent;
        auto release_position = scope_exit{[&]() noexcept {
            const auto removed = positions_.reset(position);
            assert(removed);
            if (!keep_extent) {
                positions_.trim(memory);
            }
        }};
        auto *stored = static_cast<Key *>(nullptr);
        if (position == previous_extent) {
            stored = &keys_.emplace_at(position, memory, std::forward<Source>(key));
            keep_extent = true;
        } else {
            keys_[position] = Key{std::forward<Source>(key)};
            stored = &keys_[position];
        }

        if (index_ != nullptr) {
            index_->insert(make_hash(*stored), position, memory);
        } else if (positions_.size() > IndexThreshold) {
            create_index(memory);
        }

        release_position.release();
        return {stored, true};
    }

    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    const Key *find(const Lookup &key) const {
        const auto position = find_position(key);

        return position == occupancy_index::npos ? nullptr : &keys_[position];
    }

    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    bool contains(const Lookup &key) const {
        return find(key) != nullptr;
    }

    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    bool erase(const Lookup &key, std::pmr::memory_resource &memory) {
        const auto matches = [&](const auto &stored) { return std::invoke(equal_, stored, key); };
        auto hash = linear_hash_table::hash_type{};
        auto position = occupancy_index::npos;
        if (index_ == nullptr) {
            position = find_linear_position(matches);
        } else {
            hash = make_hash(key);
            position = find_indexed_position(hash, matches);
        }
        if (position == occupancy_index::npos) {
            return false;
        }

        if (index_ != nullptr) {
            const auto removed = index_->erase(hash, position, memory);
            assert(removed);
        }

        const auto removed = positions_.reset(position);
        assert(removed);

        if (positions_.empty()) {
            release_index(memory);
            keys_.release_prefix(positions_.extent(), memory);
            positions_.release(memory);
        }

        return true;
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        release_index(memory);
        keys_.release_prefix(positions_.extent(), memory);
        positions_.release(memory);
    }

    size_type size() const noexcept { return positions_.size(); }
    bool empty() const noexcept { return positions_.empty(); }

    class iterator {
      public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = Key;
        using difference_type = std::ptrdiff_t;
        using reference = const Key &;

        iterator() = default;

        reference operator*() const noexcept { return keys_[dense_ ? dense_position_ : *position_]; }
        iterator &operator++() noexcept {
            if (dense_) {
                ++dense_position_;
            } else {
                ++position_;
            }
            return *this;
        }
        iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const iterator &other) const noexcept {
            if (dense_ != other.dense_) {
                return false;
            }
            return dense_ ? dense_position_ == other.dense_position_ : position_ == other.position_;
        }

      private:
        friend class adaptive_set;

        iterator(const adaptive_set *set, occupancy_index::iterator position)
            : position_{position}, keys_{set->keys_.accessor()} {}

        iterator(const adaptive_set *set, size_type position)
            : dense_position_{position}, keys_{set->keys_.accessor()}, dense_{true} {}

        occupancy_index::iterator position_;
        size_type dense_position_{};
        typename sparse_bunch<Key, 1>::const_accessor keys_;
        bool dense_{};
    };

    iterator begin() const noexcept {
        return positions_.dense() ? iterator{this, size_type{}} : iterator{this, positions_.begin()};
    }
    iterator end() const noexcept {
        return positions_.dense() ? iterator{this, positions_.size()} : iterator{this, positions_.end()};
    }

  private:
    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    size_type find_position(const Lookup &key) const {
        const auto matches = [&](const auto &stored) { return std::invoke(equal_, stored, key); };
        if (index_ == nullptr) {
            return find_linear_position(matches);
        }

        return find_indexed_position(make_hash(key), matches);
    }

    template <typename Predicate>
        requires std::predicate<Predicate &, const Key &>
    size_type find_linear_position(Predicate &&matches) const {
        auto keys = keys_.accessor();
        if (positions_.dense()) {
            for (auto position = size_type{}; position != positions_.size(); ++position) {
                if (std::invoke(matches, keys[position])) {
                    return position;
                }
            }
            return occupancy_index::npos;
        }

        for (auto position = size_type{}; position != positions_.extent(); ++position) {
            if (std::invoke(matches, keys[position]) && positions_.contains(position)) {
                return position;
            }
        }

        return occupancy_index::npos;
    }

    template <typename Predicate>
        requires std::predicate<Predicate &, const Key &>
    size_type find_indexed_position(linear_hash_table::hash_type hash, Predicate &&matches) const {
        assert(index_ != nullptr);
        const auto found = index_->find(hash, [&](auto position) { return std::invoke(matches, keys_[position]); });
        return found.value_or(occupancy_index::npos);
    }

    template <typename Value> linear_hash_table::hash_type make_hash(const Value &value) const {
        return linear_hash_table::hash_code(std::invoke(hash_, value));
    }

    void create_index(std::pmr::memory_resource &memory) {
        auto *storage =
            static_cast<linear_hash_table *>(memory.allocate(sizeof(linear_hash_table), alignof(linear_hash_table)));
        auto deallocate = scope_exit{
            [&]() noexcept { memory.deallocate(storage, sizeof(linear_hash_table), alignof(linear_hash_table)); }};
        auto *index = std::construct_at(storage);
        auto release = scope_exit{[&]() noexcept {
            index->release(memory);
            std::destroy_at(index);
        }};

        for (const auto position : positions_) {
            index->insert(make_hash(keys_[position]), position, memory);
        }

        index_ = index;
        release.release();
        deallocate.release();
    }

    void release_index(std::pmr::memory_resource &memory) noexcept {
        if (index_ == nullptr) {
            return;
        }

        index_->release(memory);
        std::destroy_at(index_);
        memory.deallocate(index_, sizeof(linear_hash_table), alignof(linear_hash_table));
        index_ = nullptr;
    }

    occupancy_index positions_;
    sparse_bunch<Key, 1> keys_;
    linear_hash_table *index_{};
    [[no_unique_address]] Hash hash_;
    [[no_unique_address]] Equal equal_;
};

} // namespace bg::util
