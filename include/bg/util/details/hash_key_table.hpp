#pragma once

#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <cassert>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <utility>

namespace bg::util::details {

template <typename Key, typename Hash, typename Equal> class hash_key_table {
  public:
    using position_type = std::uint32_t;

    struct insertion {
        position_type position;
        linear_hash_table::hash_type hash;
        bool inserted;
    };

    struct match {
        position_type position;
        linear_hash_table::hash_type hash;
    };

    hash_key_table() = default;

    hash_key_table(const hash_key_table &) = delete;
    hash_key_table &operator=(const hash_key_table &) = delete;
    hash_key_table(hash_key_table &&) = delete;
    hash_key_table &operator=(hash_key_table &&) = delete;

    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    [[nodiscard]] std::optional<match> find(const Lookup &key) const {
        const auto hash = make_hash(key);
        return find_with_hash(hash, [&](const auto &stored) { return std::invoke(equal_, stored, key); });
    }

    template <typename Predicate>
        requires std::predicate<Predicate &, const Key &>
    [[nodiscard]] std::optional<match> find_with(std::size_t hash, Predicate &&matches) const {
        return find_with_hash(linear_hash_table::hash_code(hash), std::forward<Predicate>(matches));
    }

    template <typename Predicate>
        requires std::predicate<Predicate &, const Key &>
    [[nodiscard]] position_type find_position_with(std::size_t hash, Predicate &&matches) const {
        const auto found = find_with(hash, std::forward<Predicate>(matches));

        return found ? found->position : occupancy_index::npos;
    }

    template <typename Predicate, typename Constructor, typename Destructor>
        requires std::predicate<Predicate &, const Key &> && std::invocable<Constructor &, Key *> &&
                 std::invocable<Destructor &, Key *>
    [[nodiscard]] insertion insert_with(std::size_t hash, Predicate &&matches, std::pmr::memory_resource &memory,
                                        Constructor &&construct, Destructor &&destroy) {
        const auto normalized_hash = linear_hash_table::hash_code(hash);
        if (const auto found =
                lookup_.find(normalized_hash, [&](auto position) { return std::invoke(matches, keys_[position]); })) {
            return {*found, normalized_hash, false};
        }

        return insert_new(normalized_hash, memory, std::forward<Constructor>(construct),
                          std::forward<Destructor>(destroy));
    }

    template <typename Predicate, typename Destructor>
        requires std::predicate<Predicate &, const Key &> && std::invocable<Destructor &, Key *>
    [[nodiscard]] bool erase_with(std::size_t hash, Predicate &&matches, std::pmr::memory_resource &memory,
                                  Destructor &&destroy) {
        const auto found = find_with(hash, std::forward<Predicate>(matches));
        if (!found) {
            return false;
        }

        erase_at_with(found->position, found->hash, memory, std::forward<Destructor>(destroy));
        return true;
    }

    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    [[nodiscard]] position_type find_position(const Lookup &key) const {
        const auto found = find(key);

        return found ? found->position : occupancy_index::npos;
    }

    template <typename Source>
        requires std::constructible_from<Key, Source> && std::invocable<const Hash &, const Source &> &&
                 std::predicate<const Equal &, const Key &, const Source &>
    [[nodiscard]] insertion insert(Source &&key, std::pmr::memory_resource &memory) {
        return insert_with(
            key, memory, [&](Key *destination) { std::construct_at(destination, std::forward<Source>(key)); },
            [](Key *value) noexcept { std::destroy_at(value); });
    }

    template <typename Lookup, typename Constructor, typename Destructor>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &> && std::invocable<Constructor &, Key *> &&
                 std::invocable<Destructor &, Key *>
    [[nodiscard]] insertion insert_with(const Lookup &key, std::pmr::memory_resource &memory, Constructor &&construct,
                                        Destructor &&destroy) {
        const auto hash = make_hash(key);
        if (const auto found =
                lookup_.find(hash, [&](auto position) { return std::invoke(equal_, keys_[position], key); })) {
            return {*found, hash, false};
        }

        return insert_new(hash, memory, std::forward<Constructor>(construct), std::forward<Destructor>(destroy));
    }

    template <typename Constructor, typename Destructor>
    [[nodiscard]] insertion insert_new(linear_hash_table::hash_type hash, std::pmr::memory_resource &memory,
                                       Constructor &&construct, Destructor &&destroy) {
        const auto position = positions_.acquire(memory);
        auto release_position = scope_exit{[&]() noexcept {
            const auto removed = positions_.reset(position);
            assert(removed);
            positions_.trim(memory);
        }};
        keys_.emplace_at_with(position, memory, construct);

        auto release_key = scope_exit{[&]() noexcept { keys_.erase_at_with(position, memory, destroy); }};
        lookup_.insert(hash, position, memory);
        release_key.release();
        release_position.release();

        return {position, hash, true};
    }

    template <typename Lookup>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &>
    [[nodiscard]] bool erase(const Lookup &key, std::pmr::memory_resource &memory) {
        const auto found = find(key);
        if (!found) {
            return false;
        }

        erase_at(found->position, found->hash, memory);

        return true;
    }

    template <typename Lookup, typename Destructor>
        requires std::invocable<const Hash &, const Lookup &> &&
                 std::predicate<const Equal &, const Key &, const Lookup &> && std::invocable<Destructor &, Key *>
    [[nodiscard]] bool erase_with(const Lookup &key, std::pmr::memory_resource &memory, Destructor &&destroy) {
        const auto found = find(key);
        if (!found) {
            return false;
        }

        erase_at_with(found->position, found->hash, memory, destroy);
        return true;
    }

    void erase_at(position_type position, linear_hash_table::hash_type hash,
                  std::pmr::memory_resource &memory) noexcept {
        erase_at_with(position, hash, memory, [](Key *value) noexcept { std::destroy_at(value); });
    }

    template <typename Destructor>
        requires std::invocable<Destructor &, Key *>
    void erase_at_with(position_type position, linear_hash_table::hash_type hash, std::pmr::memory_resource &memory,
                       Destructor &&destroy) noexcept {
        const auto removed_from_lookup = lookup_.erase(hash, position, memory);
        assert(removed_from_lookup);

        keys_.erase_at_with(position, memory, destroy);
        const auto removed_position = positions_.reset(position);
        assert(removed_position);
        positions_.trim(memory);
    }

    [[nodiscard]] bool reduce(std::pmr::memory_resource &memory) { return lookup_.reduce(memory); }

    void release(std::pmr::memory_resource &memory) noexcept {
        release_with(memory, [](Key *value) noexcept { std::destroy_at(value); });
    }

    template <typename Destructor>
        requires std::invocable<Destructor &, Key *>
    void release_with(std::pmr::memory_resource &memory, Destructor &&destroy) noexcept {
        lookup_.release(memory);
        keys_.release_with(positions_, memory, destroy);
        positions_.release(memory);
    }

    [[nodiscard]] Key &key_at(position_type position) noexcept { return keys_[position]; }
    [[nodiscard]] const Key &key_at(position_type position) const noexcept { return keys_[position]; }

    [[nodiscard]] const occupancy_index &positions() const noexcept { return positions_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return positions_.size(); }
    [[nodiscard]] bool empty() const noexcept { return positions_.empty(); }

    void swap(hash_key_table &other) noexcept(std::is_nothrow_swappable_v<Hash> && std::is_nothrow_swappable_v<Equal>) {
        positions_.swap(other.positions_);
        keys_.swap(other.keys_);
        lookup_.swap(other.lookup_);
        std::swap(hash_, other.hash_);
        std::swap(equal_, other.equal_);
    }

  private:
    template <typename Predicate>
    [[nodiscard]] std::optional<match> find_with_hash(linear_hash_table::hash_type hash, Predicate &&matches) const {
        const auto found = lookup_.find(hash, [&](auto position) { return std::invoke(matches, keys_[position]); });

        if (!found) {
            return std::nullopt;
        }

        return match{*found, hash};
    }

    template <typename Lookup> [[nodiscard]] linear_hash_table::hash_type make_hash(const Lookup &key) const {
        return linear_hash_table::hash_code(std::invoke(hash_, key));
    }

    occupancy_index positions_;
    sparse_bunch<Key> keys_;
    linear_hash_table lookup_;
    [[no_unique_address]] Hash hash_;
    [[no_unique_address]] Equal equal_;
};

} // namespace bg::util::details
