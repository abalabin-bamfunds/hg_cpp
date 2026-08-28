#pragma once

#include <bg/util/details/hash_key_table.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory_resource>
#include <utility>

namespace bg::util {

// Stores keys at stable addresses and changes capacity one bucket at a time.
// The caller must use one memory resource for all storage changes and release storage before destruction.
template <typename Key, typename Hash = std::hash<Key>, typename Equal = std::equal_to<>> class hash_set {
  public:
    using size_type = std::uint32_t;

    hash_set() = default;

    hash_set(const hash_set &) = delete;
    hash_set &operator=(const hash_set &) = delete;
    hash_set(hash_set &&) = delete;
    hash_set &operator=(hash_set &&) = delete;

    template <typename Source>
    [[nodiscard]] std::pair<const Key *, bool> insert(Source &&key, std::pmr::memory_resource &memory) {
        const auto result = keys_.insert(std::forward<Source>(key), memory);

        return {&keys_.key_at(result.position), result.inserted};
    }

    template <typename Lookup, typename Constructor, typename Destructor>
    [[nodiscard]] std::pair<const Key *, bool> insert_with(const Lookup &key, std::pmr::memory_resource &memory,
                                                           Constructor &&construct, Destructor &&destroy) {
        const auto result =
            keys_.insert_with(key, memory, std::forward<Constructor>(construct), std::forward<Destructor>(destroy));

        return {&keys_.key_at(result.position), result.inserted};
    }

    template <typename Lookup> [[nodiscard]] const Key *find(const Lookup &key) const {
        const auto position = keys_.find_position(key);

        return position == occupancy_index::npos ? nullptr : &keys_.key_at(position);
    }

    template <typename Lookup> [[nodiscard]] bool contains(const Lookup &key) const { return find(key) != nullptr; }

    template <typename Lookup> [[nodiscard]] bool erase(const Lookup &key, std::pmr::memory_resource &memory) {
        return keys_.erase(key, memory);
    }

    template <typename Lookup, typename Destructor>
    [[nodiscard]] bool erase_with(const Lookup &key, std::pmr::memory_resource &memory, Destructor &&destroy) {
        return keys_.erase_with(key, memory, std::forward<Destructor>(destroy));
    }

    [[nodiscard]] bool reduce(std::pmr::memory_resource &memory) { return keys_.reduce(memory); }

    void release(std::pmr::memory_resource &memory) noexcept { keys_.release(memory); }

    template <typename Destructor> void release_with(std::pmr::memory_resource &memory, Destructor &&destroy) noexcept {
        keys_.release_with(memory, std::forward<Destructor>(destroy));
    }

    [[nodiscard]] size_type size() const noexcept { return keys_.size(); }
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }

    void swap(hash_set &other) noexcept(noexcept(keys_.swap(other.keys_))) { keys_.swap(other.keys_); }

    class iterator {
      public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = Key;
        using difference_type = std::ptrdiff_t;
        using reference = const Key &;

        iterator() = default;

        [[nodiscard]] reference operator*() const noexcept { return set_->keys_.key_at(*position_); }
        iterator &operator++() noexcept {
            ++position_;
            return *this;
        }
        iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const iterator &) const noexcept = default;

      private:
        friend class hash_set;

        iterator(const hash_set *set, occupancy_index::iterator position) : set_{set}, position_{position} {}

        const hash_set *set_{};
        occupancy_index::iterator position_;
    };

    [[nodiscard]] iterator begin() const noexcept { return {this, keys_.positions().begin()}; }
    [[nodiscard]] iterator end() const noexcept { return {this, keys_.positions().end()}; }

  private:
    details::hash_key_table<Key, Hash, Equal> keys_;
};

} // namespace bg::util
