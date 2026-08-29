#pragma once

#include <bg/time_series/details/type_properties.hpp>

#include <type_traits>

namespace bg {

struct TS_T {};

template <typename T, auto... Properties> struct TS {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using value_type = T;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr TimeSeriesGuarantee guarantee = ParsedProperties::guarantee;
};

namespace details {

template <typename T> struct time_series_value {
    using type = void;
};

template <typename T, auto... Properties> struct time_series_value<TS<T, Properties...>> {
    using type = T;
};

template <typename T> using time_series_value_t = typename time_series_value<std::remove_cv_t<T>>::type;

} // namespace details

} // namespace bg
