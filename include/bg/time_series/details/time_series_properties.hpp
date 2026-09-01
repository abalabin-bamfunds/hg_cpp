#pragma once

#include <bg/time_series/guarantee_variable.hpp>
#include <bg/type/time_series_guarantee.hpp>

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace bg::details {

template <typename T>
inline constexpr bool is_guarantee =
    std::same_as<std::remove_cv_t<T>, TimeSeriesGuarantee> || guarantee_variable_declaration<T>;

template <auto... Properties> struct selected_time_series_guarantee;

template <> struct selected_time_series_guarantee<> {
    static constexpr auto value = TimeSeriesGuarantee::ordinary;
};

template <auto Property> struct selected_time_series_guarantee<Property> {
    static constexpr auto value = Property;
};

template <auto... Properties> struct time_series_properties {
    static_assert((... && is_guarantee<decltype(Properties)>), "unsupported time series property");
    static_assert(sizeof...(Properties) <= 1, "a time series can have only one guarantee property");

    static constexpr bool valid = true;
    static constexpr bool has_guarantee = sizeof...(Properties) == 1;
    static constexpr auto guarantee = selected_time_series_guarantee<Properties...>::value;
};

} // namespace bg::details
