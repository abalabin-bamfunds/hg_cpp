#pragma once

#include <bg/type/time_series_guarantee.hpp>

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace bg::details {

template <typename T> inline constexpr bool is_guarantee = std::same_as<std::remove_cv_t<T>, TimeSeriesGuarantee>;

template <auto... Properties> struct time_series_properties {
    static_assert((... && is_guarantee<decltype(Properties)>), "unsupported time series property");
    static_assert(sizeof...(Properties) <= 1, "a time series can have only one guarantee property");

    static constexpr bool valid = true;
    static constexpr bool has_guarantee = sizeof...(Properties) == 1;
    static constexpr TimeSeriesGuarantee guarantee = [] {
        auto result = TimeSeriesGuarantee::ordinary;
        ((result = Properties), ...);

        return result;
    }();
};

} // namespace bg::details
