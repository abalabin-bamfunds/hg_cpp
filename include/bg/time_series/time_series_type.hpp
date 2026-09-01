#pragma once

#include <bg/time_series/details/time_series_properties.hpp>
#include <bg/time_series/time_series_variable.hpp>

#include <type_traits>

namespace bg {

template <typename Target, auto... Properties> struct REF {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using target_type = Target;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

template <typename T, auto... Properties> struct TS {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using value_type = T;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

namespace details {

template <typename T> struct time_series_value {
    using type = void;
};

template <typename T, auto... Properties> struct time_series_value<TS<T, Properties...>> {
    using type = T;
};

template <typename T> using time_series_value_t = typename time_series_value<std::remove_cv_t<T>>::type;

template <typename T> struct time_series_reference : std::false_type {};

template <typename Target, auto... Properties>
struct time_series_reference<REF<Target, Properties...>> : std::true_type {
    using target_type = Target;
};

template <typename T>
inline constexpr bool is_time_series_reference = time_series_reference<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
