#pragma once

#include <bg/scalar/details/data.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T>
concept scalar_exposes_value = requires(const Scalar<T> &scalar) { scalar.value(); };

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

    if constexpr (!scalar_storage_supports_alternative_layouts<T>) {
        if (source.mode() == ScalarDataMode::source_view) {
            return *reinterpret_cast<const std::remove_cvref_t<Value> *>(source.data_word());
        }

        if (source.mode() == ScalarDataMode::inline_value) {
            if constexpr (inline_scalar_value<T>) {
                return inline_value<T>(source.data_word());
            }
            return Traits::read(*static_cast<const Stored *>(source.data()));
        }

        if (source.mode() == ScalarDataMode::shared_view || source.mode() == ScalarDataMode::shared_value) {
            if constexpr (std::is_pointer_v<Stored> && requires { Traits::uses_shared_handle; }) {
                if constexpr (Traits::uses_shared_handle) {
                    return Traits::read(reinterpret_cast<Stored>(source.data_word()));
                }
            }
        }

        return Traits::read(*static_cast<const Stored *>(source.data()));
    } else if constexpr (requires {
                             static_cast<Value (*)(const ScalarData &)>(&Traits::read_value);
                         }) {
        return Traits::read_value(source);
    } else if constexpr (std::is_reference_v<Value>) {
        static_assert(!std::is_reference_v<Value>,
                      "alternative-layout scalar storage traits with a reference value_type must provide "
                      "read_value(const ScalarData &)");
        std::terminate();
    } else {
        if constexpr (inline_scalar_value<T>) {
            if (source.mode() == ScalarDataMode::inline_value) {
                return inline_value<T>(source.data_word());
            }
        }
        if (&source.runtime_type() == &cpp_scalar_runtime_type<T>()) {
            return Traits::read(*reinterpret_cast<const Stored *>(source.data()));
        }

        alignas(Value) std::byte storage[sizeof(Value)];
        source.runtime_type().read_value(source.mode(), source.data_word(), storage);
        auto *value = reinterpret_cast<Value *>(storage);
        [[maybe_unused]] const auto cleanup = util::scope_exit{[value]() noexcept { std::destroy_at(value); }};
        return std::move(*value);
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
