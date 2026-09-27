#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/scalar/string.hpp>
#include <bg/scalar/struct/declaration.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace bg::details {

template <typename T> struct scalar_value_type {
    using type = T;
};

template <native_struct T> struct scalar_value_type<T> {
    using type = std::conditional_t<(sizeof(T) <= sizeof(std::uintptr_t)), T, const T &>;
};

template <> struct scalar_value_type<string> {
    using type = std::string_view;
};

template <typename T> struct scalar_value_type<Opaque<T>> {
    using type =
        std::conditional_t<std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(std::uintptr_t), T, const T &>;
};

template <typename T> using scalar_value_t = typename scalar_value_type<T>::type;

} // namespace bg::details
