#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace bg::details {

enum class VariadicMode : unsigned char { none, positional, named };

inline constexpr auto var_args = VariadicMode::positional;
inline constexpr auto var_kwargs = VariadicMode::named;

template <auto... Properties> consteval VariadicMode variadic_mode() {
    constexpr auto count = (std::size_t{} + ... + static_cast<std::size_t>(
                                std::same_as<std::remove_cvref_t<decltype(Properties)>, VariadicMode>));
    static_assert(count <= 1, "a parameter can declare at most one variadic property");

    VariadicMode result = VariadicMode::none;
    (([&] {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(Properties)>, VariadicMode>) {
            result = Properties;
        }
    }()), ...);
    return result;
}

} // namespace bg::details

namespace bg {

using details::var_args;
using details::var_kwargs;

} // namespace bg
