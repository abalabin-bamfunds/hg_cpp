#pragma once

#include <type_traits>

namespace bg::util {

// A type can opt in when only its default state supports byte-copy initialization.
template <typename T>
concept byte_copy_initializable =
    std::is_nothrow_default_constructible_v<T> && std::is_trivially_destructible_v<T> &&
    (std::is_trivially_copyable_v<T> || requires { requires T::supports_byte_copy_initialization; });

} // namespace bg::util
