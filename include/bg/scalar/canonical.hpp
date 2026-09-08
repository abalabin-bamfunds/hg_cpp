#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/string.hpp>

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

template <canonical_string_source T> struct canonical_scalar<T> {
    using type = string;
};

template <typename T>
    requires std::same_as<std::remove_cvref_t<T>, EvaluationTime>
struct canonical_scalar<T> {
    using type = EvaluationTime;
};

template <typename T>
    requires std::same_as<std::remove_cvref_t<T>, float> || std::same_as<std::remove_cvref_t<T>, double>
struct canonical_scalar<T> {
    using type = std::remove_cvref_t<T>;
};

} // namespace details

template <typename T> using canonical_scalar_t = typename details::canonical_scalar<T>::type;

template <typename T>
concept scalar_source = requires { typename canonical_scalar_t<T>; };

template <typename Target, typename Source>
concept scalar_source_for = std::same_as<std::remove_cvref_t<Source>, Target> ||
                            (scalar_source<Source> && std::same_as<canonical_scalar_t<Source>, Target>);

template <details::canonical_integer_source T> [[nodiscard]] constexpr std::int64_t canonical_scalar(T value) noexcept {
    return static_cast<std::int64_t>(value);
}

template <details::canonical_boolean_source T> [[nodiscard]] constexpr bool canonical_scalar(T value) noexcept {
    return value;
}

template <details::canonical_string_source T>
[[nodiscard]] constexpr std::string_view canonical_scalar(const T &value) noexcept {
    return std::string_view{value};
}

[[nodiscard]] constexpr EvaluationTime canonical_scalar(EvaluationTime value) noexcept {
    return value;
}

template <std::same_as<float> T> [[nodiscard]] constexpr float canonical_scalar(T value) noexcept {
    return value;
}

template <std::same_as<double> T> [[nodiscard]] constexpr double canonical_scalar(T value) noexcept {
    return value;
}

template <typename Target, typename Source>
    requires scalar_source_for<Target, Source>
[[nodiscard]] constexpr decltype(auto) scalar_source_value(Source &&source) noexcept {
    if constexpr (std::same_as<std::remove_cvref_t<Source>, Target>) {
        return std::forward<Source>(source);
    } else {
        return canonical_scalar(std::forward<Source>(source));
    }
}

} // namespace bg
