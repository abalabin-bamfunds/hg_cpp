#pragma once

#include <bg/scalar/details/hash.hpp>
#include <bg/scalar/details/storage_traits_base.hpp>
#include <bg/scalar/type.hpp>
#include <bg/util/hash_dictionary.hpp>

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace bg {

template <typename K, typename V> class MutableDict;

namespace details {

template <typename K, typename V> struct scalar_dict_storage {
    using KeyTraits = scalar_storage_traits<K>;
    using ValueTraits = scalar_storage_traits<V>;
    using StoredKey = typename KeyTraits::stored_type;
    using StoredValue = typename ValueTraits::stored_type;
    using Items = util::hash_dictionary<StoredKey, StoredValue, scalar_hash<K>, scalar_equal<K>>;

    static constexpr bool supports_byte_copy_initialization = true;

    Items items;
};

} // namespace details

template <typename K, typename V> class Dict {
    using Storage = details::scalar_dict_storage<K, V>;
    using KeyTraits = details::scalar_storage_traits<K>;
    using ValueTraits = details::scalar_storage_traits<V>;

  public:
    using key_type = K;
    using mapped_type = V;
    using key_value_type = details::scalar_value_t<K>;
    using mapped_value_type = details::scalar_value_t<V>;

    Dict() = delete;

  private:
    template <typename> friend struct details::scalar_storage_traits;
    friend class MutableDict<K, V>;

    std::size_t size() const noexcept { return storage_->items.size(); }
    bool empty() const noexcept { return storage_->items.empty(); }
    bool contains(key_value_type key) const { return storage_->items.contains(key); }

    mapped_value_type at(key_value_type key) const {
        const auto *value = storage_->items.find(key);
        if (value == nullptr) {
            throw std::out_of_range{"dictionary key was not found"};
        }

        return ValueTraits::read(*value);
    }

    struct entry {
        key_value_type key;
        mapped_value_type value;
    };

    class iterator {
      public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = entry;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        value_type operator*() const noexcept {
            const auto [key, value] = *position_;
            return {KeyTraits::read(key), ValueTraits::read(value)};
        }
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
        friend Dict;

        explicit iterator(typename Storage::Items::const_iterator position) : position_{position} {}

        typename Storage::Items::const_iterator position_;
    };

    iterator begin() const noexcept { return iterator{storage_->items.begin()}; }
    iterator end() const noexcept { return iterator{storage_->items.end()}; }

    friend bool operator==(const Dict &left, const Dict &right) {
        if (left.size() != right.size()) {
            return false;
        }

        for (const auto [key, value] : left) {
            if (!right.contains(key) || right.at(key) != value) {
                return false;
            }
        }

        return true;
    }

    explicit Dict(const Storage &storage) noexcept : storage_(&storage) {}

    const Storage &storage() const noexcept { return *storage_; }

    const Storage *storage_{};
};

template <details::scalar_variable K, details::scalar_variable V> class Dict<K, V> {
  public:
    Dict() = delete;

    using key_type = K;
    using mapped_type = V;
};

template <typename K, details::scalar_variable V> class Dict<K, V> {
  public:
    Dict() = delete;

    using key_type = K;
    using mapped_type = V;
};

template <details::scalar_variable K, typename V> class Dict<K, V> {
  public:
    Dict() = delete;

    using key_type = K;
    using mapped_type = V;
};

template <typename T> struct is_dict : std::false_type {};
template <typename K, typename V> struct is_dict<Dict<K, V>> : std::true_type {};
template <typename T> inline constexpr bool is_dict_v = is_dict<T>::value;

} // namespace bg

namespace bg::details {

template <typename K, typename V>
inline constexpr bool generic_scalar<Dict<K, V>> = generic_scalar<K> || generic_scalar<V>;

} // namespace bg::details
