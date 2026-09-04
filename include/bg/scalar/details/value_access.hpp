#pragma once

#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/storage_traits_base.hpp>
#include <bg/scalar/runtime_type.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace bg::details {

template <typename T>
inline constexpr bool inline_scalar_value =
    std::same_as<scalar_stored_t<T>, scalar_value_t<T>> && std::is_trivially_copyable_v<scalar_stored_t<T>> &&
    sizeof(scalar_stored_t<T>) <= sizeof(std::uintptr_t);

template <typename T> [[nodiscard]] scalar_value_t<T> inline_value(std::uintptr_t data) noexcept {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;

    auto bytes = std::array<std::byte, sizeof(Stored)>{};
    std::memcpy(bytes.data(), std::addressof(data), bytes.size());
    return Traits::read(std::bit_cast<Stored>(bytes));
}

template <typename T> scalar_value_t<T> scalar_data_value(const ScalarData &source) {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = scalar_value_t<T>;

    if constexpr (inline_scalar_value<T>) {
        if (source.mode() == ScalarDataMode::inline_value) {
            return inline_value<T>(source.data_word());
        }
    }
    if (source.mode() == ScalarDataMode::source_view) {
        return *reinterpret_cast<const std::remove_cvref_t<Value> *>(source.data_word());
    }
    if (&source.runtime_type() == &cpp_scalar_runtime_type<T>()) {
        return Traits::read(*reinterpret_cast<const Stored *>(source.data_word()));
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

} // namespace bg::details
