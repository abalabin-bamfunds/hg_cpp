#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/type.hpp>

#include <cstddef>
#include <functional>
#include <type_traits>

namespace bg {

template <typename T> class List {
    using ItemTraits = details::scalar_storage_traits<T>;

  public:
    using item_type = T;
    using value_type = details::scalar_value_t<T>;

    List() = default;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] value_type operator[](std::size_t index) const noexcept { return ItemTraits::read(data_[index]); }

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

  private:
    template <typename> friend struct details::scalar_storage_traits;
    template <typename> friend class TypedScalarValueInputView;

    List(const typename ItemTraits::stored_type *data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] const void *item_data(std::size_t index) const noexcept { return data_ + index; }

    const typename ItemTraits::stored_type *data_{};
    std::size_t size_{};
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

static_assert(sizeof(bg::List<int>) == 2 * sizeof(void *));
