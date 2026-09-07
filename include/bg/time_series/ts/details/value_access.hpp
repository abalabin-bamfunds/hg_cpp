#pragma once

#include <bg/scalar/details/value_type.hpp>

#include <memory>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T>
concept scalar_value_supports_arrow =
    std::is_pointer_v<std::remove_cvref_t<scalar_value_t<T>>> ||
    std::is_class_v<std::remove_cvref_t<scalar_value_t<T>>> ||
    std::is_union_v<std::remove_cvref_t<scalar_value_t<T>>>;

template <typename T> class ScalarValueArrowProxy {
  public:
    explicit ScalarValueArrowProxy(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_{std::move(value)} {}

    const T *operator->() const noexcept { return std::addressof(value_); }

  private:
    T value_;
};

template <typename T> [[nodiscard]] auto scalar_value_arrow(scalar_value_t<T> value) noexcept {
    using Value = scalar_value_t<T>;

    if constexpr (std::is_pointer_v<std::remove_cvref_t<Value>>) {
        return value;
    } else if constexpr (std::is_lvalue_reference_v<Value>) {
        return std::addressof(value);
    } else {
        return ScalarValueArrowProxy<Value>{std::move(value)};
    }
}

} // namespace bg::details
