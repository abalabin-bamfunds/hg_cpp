#pragma once

#include <bg/scalar/details/data.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/runtime_type.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T>
inline constexpr bool inline_scalar_value =
    std::same_as<scalar_stored_t<T>, typename scalar_storage_traits<T>::value_type> &&
    std::is_trivially_copyable_v<scalar_stored_t<T>> && sizeof(scalar_stored_t<T>) <= sizeof(std::uintptr_t);

template <typename T> typename scalar_storage_traits<T>::value_type inline_value(std::uintptr_t data) noexcept {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    auto bytes = std::array<std::byte, sizeof(Stored)>{};
    std::memcpy(bytes.data(), std::addressof(data), bytes.size());
    return Traits::read(std::bit_cast<Stored>(bytes));
}

template <typename T> typename scalar_storage_traits<T>::value_type scalar_data_value(const ScalarData &source) {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;

    if constexpr (inline_scalar_value<T>) {
        if (source.mode() == ScalarDataMode::inline_value) {
            return inline_value<T>(source.data_word());
        }
    }
    if (source.mode() == ScalarDataMode::source_view) {
        return *reinterpret_cast<const std::remove_cvref_t<Value> *>(source.data_word());
    }
    if (&source.runtime_type() == &cpp_scalar_runtime_type<T>()) {
        return Traits::read(*reinterpret_cast<const Stored *>(source.data()));
    }

    if constexpr (std::is_reference_v<Value>) {
        return *static_cast<const std::remove_cvref_t<Value> *>(source.data());
    } else {
        alignas(Value) std::byte storage[sizeof(Value)];
        source.runtime_type().read_value(source.mode(), source.data_word(), storage);
        auto *value = reinterpret_cast<Value *>(storage);
        auto result = std::move(*value);
        std::destroy_at(value);
        return result;
    }
}

template <typename T>
concept scalar_value_supports_arrow =
    std::is_pointer_v<std::remove_cvref_t<typename scalar_storage_traits<T>::value_type>> ||
    std::is_class_v<std::remove_cvref_t<typename scalar_storage_traits<T>::value_type>> ||
    std::is_union_v<std::remove_cvref_t<typename scalar_storage_traits<T>::value_type>>;

template <typename T> class ScalarValueArrowProxy {
  public:
    explicit ScalarValueArrowProxy(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_{std::move(value)} {}

    const T *operator->() const noexcept { return std::addressof(value_); }

  private:
    T value_;
};

template <typename T>
[[nodiscard]] auto scalar_value_arrow(typename scalar_storage_traits<T>::value_type value) noexcept {
    using Value = typename scalar_storage_traits<T>::value_type;

    if constexpr (std::is_pointer_v<std::remove_cvref_t<Value>>) {
        return value;
    } else if constexpr (std::is_lvalue_reference_v<Value>) {
        return std::addressof(value);
    } else {
        return ScalarValueArrowProxy<Value>{std::move(value)};
    }
}

} // namespace bg::details
