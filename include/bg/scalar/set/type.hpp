#pragma once

#include <bg/scalar/details/hash.hpp>
#include <bg/scalar/details/storage_traits_base.hpp>
#include <bg/scalar/type.hpp>
#include <bg/util/hash_set.hpp>

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace bg {

template <typename T> class MutableSet;

namespace details {

template <typename T> struct scalar_set_storage {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Items = util::hash_set<Stored, scalar_hash<T>, scalar_equal<T>>;

    Items items;
};

} // namespace details

template <typename T> class Set {
    using Storage = details::scalar_set_storage<T>;
    using ItemTraits = details::scalar_storage_traits<T>;

  public:
    using item_type = T;
    using value_type = details::scalar_value_t<T>;

    Set() = delete;

  private:
    template <typename> friend struct details::scalar_storage_traits;
    friend class MutableSet<T>;

    [[nodiscard]] std::size_t size() const noexcept { return storage_->items.size(); }
    [[nodiscard]] bool empty() const noexcept { return storage_->items.empty(); }
    [[nodiscard]] bool contains(value_type value) const { return storage_->items.contains(value); }

    class iterator {
      public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = details::scalar_value_t<T>;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        [[nodiscard]] value_type operator*() const noexcept { return ItemTraits::read(*position_); }
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
        friend Set;

        explicit iterator(typename Storage::Items::iterator position) : position_{position} {}

        typename Storage::Items::iterator position_;
    };

    [[nodiscard]] iterator begin() const noexcept { return iterator{storage_->items.begin()}; }
    [[nodiscard]] iterator end() const noexcept { return iterator{storage_->items.end()}; }

    friend bool operator==(const Set &left, const Set &right) {
        if (left.size() != right.size()) {
            return false;
        }

        for (const auto item : left) {
            if (!right.contains(item)) {
                return false;
            }
        }

        return true;
    }

    explicit Set(const Storage &storage) noexcept : storage_(&storage) {}

    [[nodiscard]] const Storage &storage() const noexcept { return *storage_; }

    const Storage *storage_{};
};

template <details::scalar_variable Variable> class Set<Variable> {
  public:
    Set() = delete;

    using item_type = Variable;
};

template <typename T> struct is_set : std::false_type {};
template <typename T> struct is_set<Set<T>> : std::true_type {};
template <typename T> inline constexpr bool is_set_v = is_set<T>::value;

} // namespace bg
