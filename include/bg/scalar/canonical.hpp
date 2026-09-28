#pragma once

#include <bg/scalar/builtins.hpp>
#include <bg/scalar/struct/declaration.hpp>

#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace bg {

namespace details {

template <typename T>
concept canonical_integer_source =
    std::integral<std::remove_cvref_t<T>> && !std::same_as<std::remove_cvref_t<T>, bool> &&
    !std::same_as<std::remove_cvref_t<T>, char> && !std::same_as<std::remove_cvref_t<T>, signed char> &&
    !std::same_as<std::remove_cvref_t<T>, unsigned char> &&
    (std::numeric_limits<std::remove_cvref_t<T>>::digits <= std::numeric_limits<std::int64_t>::digits);

template <typename T>
concept canonical_boolean_source = std::same_as<std::remove_cvref_t<T>, bool>;

template <typename T>
concept canonical_exact_source =
    std::same_as<std::remove_cvref_t<T>, byte> || std::same_as<std::remove_cvref_t<T>, timedelta_t> ||
    std::same_as<std::remove_cvref_t<T>, symbol>;

template <typename T> struct is_character_string : std::false_type {};

template <typename Traits, typename Allocator>
struct is_character_string<std::basic_string<char, Traits, Allocator>> : std::true_type {};

template <typename T>
concept canonical_string_source =
    std::same_as<std::remove_cvref_t<T>, std::string_view> || is_character_string<std::remove_cvref_t<T>>::value ||
    (std::is_array_v<std::remove_reference_t<T>> &&
     std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>);

template <typename T> struct canonical_scalar;

template <canonical_integer_source T> struct canonical_scalar<T> {
    using type = std::int64_t;
};

template <canonical_boolean_source T> struct canonical_scalar<T> {
    using type = bool;
};

template <canonical_exact_source T> struct canonical_scalar<T> {
    using type = std::remove_cvref_t<T>;
};

template <canonical_string_source T> struct canonical_scalar<T> {
    using type = string;
};

template <typename T>
    requires std::same_as<std::remove_cvref_t<T>, date_time_t>
struct canonical_scalar<T> {
    using type = date_time_t;
};

template <typename T>
    requires std::same_as<std::remove_cvref_t<T>, float> || std::same_as<std::remove_cvref_t<T>, double>
struct canonical_scalar<T> {
    using type = std::remove_cvref_t<T>;
};

template <typename T>
    requires native_struct<std::remove_cvref_t<T>>
struct canonical_scalar<T> {
    using type = std::remove_cvref_t<T>;
};

} // namespace details

template <typename T> using canonical_scalar_t = typename details::canonical_scalar<T>::type;

template <typename T>
concept scalar_source = requires { typename canonical_scalar_t<T>; };

namespace details {

template <typename T, bool = scalar_source<T>> struct canonical_scalar_type {
    using type = std::remove_cvref_t<T>;
};

template <typename T> struct canonical_scalar_type<T, true> {
    using type = canonical_scalar_t<T>;
};

template <typename T> using canonical_scalar_type_t = typename canonical_scalar_type<T>::type;

template <typename Target, typename Source>
    requires std::constructible_from<std::remove_cvref_t<Target>, Source>
constexpr auto scalar_api_value(Source &&source)
    noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Target>, Source>) {
    return std::remove_cvref_t<Target>(std::forward<Source>(source));
}

} // namespace details

template <typename Target, typename Source>
concept scalar_source_for = std::same_as<std::remove_cvref_t<Source>, Target> ||
                            (scalar_source<Source> && std::same_as<canonical_scalar_t<Source>, Target>);

template <details::canonical_integer_source T> constexpr std::int64_t canonical_scalar(T value) noexcept {
    return static_cast<std::int64_t>(value);
}

template <details::canonical_boolean_source T> constexpr bool canonical_scalar(T value) noexcept {
    return value;
}

template <details::canonical_exact_source T>
constexpr std::remove_cvref_t<T> canonical_scalar(T value) noexcept {
    return value;
}

template <details::canonical_string_source T>
constexpr std::string_view canonical_scalar(const T &value) noexcept {
    return std::string_view{value};
}

constexpr date_time_t canonical_scalar(date_time_t value) noexcept {
    return value;
}

template <std::same_as<float> T> constexpr float canonical_scalar(T value) noexcept {
    return value;
}

template <std::same_as<double> T> constexpr double canonical_scalar(T value) noexcept {
    return value;
}

template <typename T>
    requires native_struct<std::remove_cvref_t<T>>
constexpr decltype(auto) canonical_scalar(T &&value) noexcept {
    return std::forward<T>(value);
}

template <typename Target, typename Source>
    requires scalar_source_for<Target, Source>
constexpr decltype(auto) scalar_source_value(Source &&source) noexcept {
    if constexpr (std::same_as<std::remove_cvref_t<Source>, Target>) {
        return std::forward<Source>(source);
    } else {
        return canonical_scalar(std::forward<Source>(source));
    }
}

} // namespace bg
