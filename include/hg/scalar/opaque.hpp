#pragma once

#include <concepts>
#include <type_traits>

namespace bg {

template <typename T> struct Opaque {
    static_assert(std::copy_constructible<T>);
    static_assert(std::destructible<T>);

    using value_type = T;
};

template <typename T> struct is_opaque : std::false_type {};

template <typename T> struct is_opaque<Opaque<T>> : std::true_type {};

template <typename T> inline constexpr bool is_opaque_v = is_opaque<std::remove_cvref_t<T>>::value;

} // namespace bg
