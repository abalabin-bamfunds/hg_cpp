#pragma once

#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/type.hpp>
#include <bg/util/contiguous_bunch.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace bg {

template <typename T> class MutableList;

namespace details {

template <typename T> struct scalar_list_storage {
    using Stored = scalar_stored_t<T>;

    static constexpr bool supports_byte_copy_initialization = true;

    util::contiguous_bunch<Stored> items;
};

} // namespace details

template <typename T> class List {
    using ItemTraits = scalar_storage_traits<T>;
    using Storage = details::scalar_list_storage<T>;

  public:
    using item_type = T;
    using value_type = details::scalar_value_t<T>;

    List() = delete;

  private:
    template <typename> friend struct scalar_storage_traits;
    template <typename> friend class Scalar;
    friend class MutableList<T>;

    std::size_t size() const noexcept { return storage_->items.size(); }
    bool empty() const noexcept { return storage_->items.empty(); }

    value_type operator[](std::size_t index) const noexcept {
        return ItemTraits::read(storage_->items[static_cast<std::uint32_t>(index)]);
    }

    friend bool operator==(const List &left, const List &right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }

        for (auto i = std::size_t{}; i < left.size(); ++i) {
            if (!ItemTraits::equals(left[i], right[i])) {
                return false;
            }
        }

        return true;
    }

    explicit List(const Storage &storage) noexcept : storage_(&storage) {}

    const Storage &storage() const noexcept { return *storage_; }

    const void *item_data(std::size_t index) const noexcept { return &storage_->items[index]; }

    const Storage *storage_{};
};

template <details::scalar_variable Variable> class List<Variable> {
  public:
    List() = delete;

    using item_type = Variable;
};

template <typename T> struct is_list : std::false_type {};
template <typename T> struct is_list<List<T>> : std::true_type {};

template <typename T> inline constexpr bool is_list_v = is_list<T>::value;

} // namespace bg

namespace bg::details {

template <typename T> inline constexpr bool generic_scalar<List<T>> = generic_scalar<T>;

} // namespace bg::details

template <typename T> struct std::hash<bg::List<T>> {
    std::size_t operator()(const bg::List<T> &value) const noexcept {
        return bg::scalar_storage_traits<bg::List<T>>::hash(value);
    }
};

static_assert(sizeof(bg::List<int>) == sizeof(void *));
