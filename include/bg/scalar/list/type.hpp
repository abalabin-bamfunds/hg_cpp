#pragma once

#include <bg/scalar/details/storage_traits_base.hpp>
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

    util::contiguous_bunch<Stored> items;
};

} // namespace details

template <typename T> class List {
    using ItemTraits = details::scalar_storage_traits<T>;
    using Storage = details::scalar_list_storage<T>;

  public:
    using item_type = T;
    using value_type = details::scalar_value_t<T>;

    List() = delete;

  private:
    template <typename> friend struct details::scalar_storage_traits;
    template <typename> friend class TypedScalarValueInputView;
    friend class MutableList<T>;

    [[nodiscard]] std::size_t size() const noexcept { return storage_->items.size(); }
    [[nodiscard]] bool empty() const noexcept { return storage_->items.empty(); }

    [[nodiscard]] value_type operator[](std::size_t index) const noexcept {
        return ItemTraits::read(storage_->items[static_cast<std::uint32_t>(index)]);
    }

    friend bool operator==(const List &left, const List &right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }

        for (auto i = std::size_t{}; i < left.size(); ++i) {
            if (left[i] != right[i]) {
                return false;
            }
        }

        return true;
    }

    explicit List(const Storage &storage) noexcept : storage_(&storage) {}

    [[nodiscard]] const Storage &storage() const noexcept { return *storage_; }

    [[nodiscard]] const void *item_data(std::size_t index) const noexcept { return &storage_->items[index]; }

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

template <typename T> struct std::hash<bg::List<T>> {
    [[nodiscard]] std::size_t operator()(const bg::List<T> &value) const noexcept {
        return bg::details::scalar_storage_traits<bg::List<T>>::hash(value);
    }
};

static_assert(sizeof(bg::List<int>) == sizeof(void *));
