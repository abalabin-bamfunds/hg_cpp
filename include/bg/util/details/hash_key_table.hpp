#pragma once

#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <cassert>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
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
        const auto found = lookup_.find(hash, [&](auto position) { return std::invoke(equal_, keys_[position], key); });

        if (!found) {
            return std::nullopt;
        }

        return match{*found, hash};
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
        const auto hash = make_hash(key);
        if (const auto found = lookup_.find(
                hash, [&](auto position) { return std::invoke(equal_, keys_[position], key); })) {
            return {*found, hash, false};
        }

        const auto position = positions_.acquire(memory);
        try {
            keys_.emplace_at(position, memory, std::forward<Source>(key));
        } catch (...) {
            const auto removed = positions_.reset(position);
            assert(removed);
            positions_.trim(memory);
            throw;
        }

        try {
            lookup_.insert(hash, position, memory);
        } catch (...) {
            keys_.erase_at(position, memory);
            const auto removed = positions_.reset(position);
            assert(removed);
            positions_.trim(memory);
            throw;
        }

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

    void erase_at(position_type position, linear_hash_table::hash_type hash,
                  std::pmr::memory_resource &memory) noexcept {
        const auto removed_from_lookup = lookup_.erase(hash, position, memory);
        assert(removed_from_lookup);

        keys_.erase_at(position, memory);
        const auto removed_position = positions_.reset(position);
        assert(removed_position);
        positions_.trim(memory);
    }

    [[nodiscard]] bool reduce(std::pmr::memory_resource &memory) { return lookup_.reduce(memory); }

    void release(std::pmr::memory_resource &memory) noexcept {
        lookup_.release(memory);
        keys_.release(positions_, memory);
        positions_.release(memory);
    }

    [[nodiscard]] Key &key_at(position_type position) noexcept { return keys_[position]; }
    [[nodiscard]] const Key &key_at(position_type position) const noexcept { return keys_[position]; }

    [[nodiscard]] const occupancy_index &positions() const noexcept { return positions_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return positions_.size(); }
    [[nodiscard]] bool empty() const noexcept { return positions_.empty(); }

  private:
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
