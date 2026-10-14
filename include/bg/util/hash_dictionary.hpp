#pragma once

#include <bg/util/details/hash_key_table.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::util {

// Stores stable-address keys and values in separate typed columns that share occupied positions.
// The caller must use one memory resource for all storage changes and release storage before destruction.
template <typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<>>
class hash_dictionary {
  public:
    using size_type = std::uint32_t;

    hash_dictionary() = default;

    hash_dictionary(const hash_dictionary &) = delete;
    hash_dictionary &operator=(const hash_dictionary &) = delete;
    hash_dictionary(hash_dictionary &&) = delete;
    hash_dictionary &operator=(hash_dictionary &&) = delete;

    template <typename KeySource, typename... ValueArguments>
    std::pair<Value *, bool> try_emplace(KeySource &&key, std::pmr::memory_resource &memory,
                                         ValueArguments &&...value_arguments) {
        const auto result = keys_.insert(std::forward<KeySource>(key), memory);
        if (!result.inserted) {
            return {&values_[result.position], false};
        }

        auto rollback = scope_exit{[&]() noexcept { keys_.erase_at(result.position, result.hash, memory); }};
        auto &value = values_.emplace_at(result.position, memory, std::forward<ValueArguments>(value_arguments)...);
        rollback.release();
        return {&value, true};
    }

    template <typename KeyLookup, typename KeyConstructor, typename KeyDestructor, typename ValueConstructor>
    std::pair<Value *, bool> try_emplace_with(KeyLookup const &key, std::pmr::memory_resource &memory,
                                              KeyConstructor &&construct_key, KeyDestructor &&destroy_key,
                                              ValueConstructor &&construct_value) {
        const auto result = keys_.insert_with(key, memory, std::forward<KeyConstructor>(construct_key), destroy_key);
        if (!result.inserted) {
            return {&values_[result.position], false};
        }

        auto rollback =
            scope_exit{[&]() noexcept { keys_.erase_at_with(result.position, result.hash, memory, destroy_key); }};
        auto &value = values_.emplace_at_with(result.position, memory, construct_value);
        rollback.release();
        return {&value, true};
    }

    template <typename Predicate, typename KeyConstructor, typename KeyDestructor, typename ValueConstructor>
    std::pair<Value *, bool> try_emplace_with(std::size_t hash, Predicate &&matches, std::pmr::memory_resource &memory,
                                              KeyConstructor &&construct_key, KeyDestructor &&destroy_key,
                                              ValueConstructor &&construct_value) {
        const auto result = keys_.insert_with(hash, std::forward<Predicate>(matches), memory,
                                              std::forward<KeyConstructor>(construct_key), destroy_key);
        if (!result.inserted) {
            return {&values_[result.position], false};
        }

        auto rollback =
            scope_exit{[&]() noexcept { keys_.erase_at_with(result.position, result.hash, memory, destroy_key); }};
        auto &value = values_.emplace_at_with(result.position, memory, construct_value);
        rollback.release();
        return {&value, true};
    }

    template <typename HashFunction, typename Predicate, typename KeyConstructor, typename KeyDestructor,
              typename ValueConstructor>
    std::pair<Value *, bool> try_emplace_constructed_with(
        std::pmr::memory_resource &memory, HashFunction &&hash_key, Predicate &&matches,
        KeyConstructor &&construct_key, KeyDestructor &&destroy_key, ValueConstructor &&construct_value) {
        const auto result = keys_.insert_constructed_with(
            memory, std::forward<HashFunction>(hash_key), std::forward<Predicate>(matches),
            std::forward<KeyConstructor>(construct_key), destroy_key);
        if (!result.inserted) {
            return {&values_[result.position], false};
        }

        auto rollback =
            scope_exit{[&]() noexcept { keys_.erase_at_with(result.position, result.hash, memory, destroy_key); }};
        auto &value = values_.emplace_at_with(result.position, memory, std::forward<ValueConstructor>(construct_value));
        rollback.release();
        return {&value, true};
    }

    template <typename KeySource, typename ValueSource>
    Value &insert_or_assign(KeySource &&key, ValueSource &&value, std::pmr::memory_resource &memory) {
        auto [stored, inserted] = try_emplace(std::forward<KeySource>(key), memory, std::forward<ValueSource>(value));
        if (!inserted) {
            *stored = std::forward<ValueSource>(value);
        }

        return *stored;
    }

    template <typename Lookup> Value *find(const Lookup &key) {
        const auto position = keys_.find_position(key);

        return position == occupancy_index::npos ? nullptr : &values_[position];
    }

    template <typename Lookup> const Value *find(const Lookup &key) const {
        const auto position = keys_.find_position(key);

        return position == occupancy_index::npos ? nullptr : &values_[position];
    }

    template <typename Lookup> const Key *find_key(const Lookup &key) const {
        const auto position = keys_.find_position(key);

        return position == occupancy_index::npos ? nullptr : &keys_.key_at(position);
    }

    template <typename Lookup> size_type find_position(const Lookup &key) const { return keys_.find_position(key); }

    Key &key_at(size_type position) noexcept { return keys_.key_at(position); }
    const Key &key_at(size_type position) const noexcept { return keys_.key_at(position); }
    Value &value_at(size_type position) noexcept { return values_[position]; }
    const Value &value_at(size_type position) const noexcept { return values_[position]; }

    template <typename Lookup> bool contains(const Lookup &key) const { return find(key) != nullptr; }

    template <typename Predicate> Value *find_with(std::size_t hash, Predicate &&matches) {
        const auto position = keys_.find_position_with(hash, std::forward<Predicate>(matches));

        return position == occupancy_index::npos ? nullptr : &values_[position];
    }

    template <typename Predicate> const Value *find_with(std::size_t hash, Predicate &&matches) const {
        const auto position = keys_.find_position_with(hash, std::forward<Predicate>(matches));

        return position == occupancy_index::npos ? nullptr : &values_[position];
    }

    template <typename Lookup> Value &at(const Lookup &key) {
        auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"dictionary key was not found"};
        }

        return *value;
    }

    template <typename Lookup> const Value &at(const Lookup &key) const {
        const auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"dictionary key was not found"};
        }

        return *value;
    }

    template <typename Lookup> bool erase(const Lookup &key, std::pmr::memory_resource &memory) {
        const auto found = keys_.find(key);
        if (!found) {
            return false;
        }

        values_.erase_at(found->position, memory);
        keys_.erase_at(found->position, found->hash, memory);

        return true;
    }

    template <typename Lookup, typename KeyDestructor, typename ValueDestructor>
    bool erase_with(const Lookup &key, std::pmr::memory_resource &memory, KeyDestructor &&destroy_key,
                    ValueDestructor &&destroy_value) {
        const auto found = keys_.find(key);
        if (!found) {
            return false;
        }

        values_.erase_at_with(found->position, memory, destroy_value);
        keys_.erase_at_with(found->position, found->hash, memory, destroy_key);
        return true;
    }

    template <typename Predicate, typename KeyDestructor, typename ValueDestructor>
    bool erase_with(std::size_t hash, Predicate &&matches, std::pmr::memory_resource &memory,
                    KeyDestructor &&destroy_key, ValueDestructor &&destroy_value) {
        const auto found = keys_.find_with(hash, std::forward<Predicate>(matches));
        if (!found) {
            return false;
        }

        values_.erase_at_with(found->position, memory, destroy_value);
        keys_.erase_at_with(found->position, found->hash, memory, destroy_key);
        return true;
    }

    bool reduce(std::pmr::memory_resource &memory) { return keys_.reduce(memory); }

    void release(std::pmr::memory_resource &memory) noexcept {
        values_.release(keys_.positions(), memory);
        keys_.release(memory);
    }

    template <typename KeyDestructor, typename ValueDestructor>
    void release_with(std::pmr::memory_resource &memory, KeyDestructor &&destroy_key,
                      ValueDestructor &&destroy_value) noexcept {
        values_.release_with(keys_.positions(), memory, destroy_value);
        keys_.release_with(memory, destroy_key);
    }

    size_type size() const noexcept { return keys_.size(); }
    bool empty() const noexcept { return keys_.empty(); }

    void swap(hash_dictionary &other) noexcept(noexcept(keys_.swap(other.keys_))) {
        keys_.swap(other.keys_);
        values_.swap(other.values_);
    }

    struct entry {
        const Key &key;
        Value &value;
    };

    struct const_entry {
        const Key &key;
        const Value &value;
    };

    template <bool Constant> class basic_iterator {
      public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::conditional_t<Constant, const_entry, entry>;
        using difference_type = std::ptrdiff_t;

        basic_iterator() = default;

        basic_iterator(const basic_iterator<false> &other)
            requires Constant
            : dictionary_{other.dictionary_}, position_{other.position_} {}

        value_type operator*() const noexcept {
            const auto position = *position_;
            return {dictionary_->keys_.key_at(position), dictionary_->values_[position]};
        }
        basic_iterator &operator++() noexcept {
            ++position_;
            return *this;
        }
        basic_iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const basic_iterator &) const noexcept = default;

      private:
        friend class hash_dictionary;
        template <bool> friend class basic_iterator;

        using dictionary_type = std::conditional_t<Constant, const hash_dictionary, hash_dictionary>;

        basic_iterator(dictionary_type *dictionary, occupancy_index::iterator position)
            : dictionary_{dictionary}, position_{position} {}

        dictionary_type *dictionary_{};
        occupancy_index::iterator position_;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    iterator begin() noexcept { return {this, keys_.positions().begin()}; }
    iterator end() noexcept { return {this, keys_.positions().end()}; }
    const_iterator begin() const noexcept { return {this, keys_.positions().begin()}; }
    const_iterator end() const noexcept { return {this, keys_.positions().end()}; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

  private:
    details::hash_key_table<Key, Hash, Equal> keys_;
    sparse_bunch<Value> values_;
};

} // namespace bg::util
