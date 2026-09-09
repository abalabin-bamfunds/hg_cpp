#pragma once

#include <bg/util/compile_time_string.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace bg::details {

template <util::compile_time_string Name, typename Value> struct named_argument {
    static constexpr auto name = Name;

    Value value;
};

template <util::compile_time_string Name> struct compile_time_name {
    static constexpr auto text = Name;

    template <typename Value> [[nodiscard]] constexpr auto operator=(Value &&value) const {
        return named_argument<Name, std::decay_t<Value>>{std::forward<Value>(value)};
    }
};

template <typename T> struct is_named_argument : std::false_type {};

template <util::compile_time_string Name, typename Value>
struct is_named_argument<named_argument<Name, Value>> : std::true_type {};

template <typename T> inline constexpr bool is_named_argument_v = is_named_argument<std::remove_cvref_t<T>>::value;

template <util::compile_time_string Name, typename T>
inline constexpr bool named_argument_is = [] {
    using Argument = std::remove_cvref_t<T>;

    if constexpr (is_named_argument_v<Argument>) {
        return Argument::name == Name;
    } else {
        return false;
    }
}();

template <util::compile_time_string Name, typename... Arguments>
inline constexpr std::size_t named_argument_count =
    (std::size_t{} + ... + static_cast<std::size_t>(named_argument_is<Name, Arguments>));

template <util::compile_time_string Name, typename First, typename... Rest>
decltype(auto) named_argument_value(First &&first, Rest &&...rest) {
    if constexpr (named_argument_is<Name, First>) {
        return std::forward<First>(first).value;
    } else {
        return named_argument_value<Name>(std::forward<Rest>(rest)...);
    }
}

} // namespace bg::details

namespace bg::literals {

template <util::compile_time_string Name> [[nodiscard]] consteval auto operator""_n() {
    return details::compile_time_name<Name>{};
}

} // namespace bg::literals
