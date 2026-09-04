#pragma once

#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/guarantee_variable_instance.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <cstddef>
#include <type_traits>

namespace bg::details {

template <typename T>
concept has_cpp_scalar_type = requires { cpp_scalar_type<T>(); };

// Defines whether a C++ time series declaration is supported or generic and produces its semantic type.
template <typename TimeSeries> struct time_series_type_traits {
    static constexpr bool generic = time_series_variable<TimeSeries>;
    static constexpr bool supported = time_series_variable<TimeSeries>;
};

template <typename T>
concept generic_time_series = time_series_type_traits<std::remove_cv_t<T>>::generic;

template <typename T>
concept supported_time_series = time_series_type_traits<std::remove_cv_t<T>>::supported;

template <typename Scope, std::size_t Index, bool Input, typename TimeSeries>
[[nodiscard]] const TimeSeriesType &time_series_type_for(GuaranteeParameter inherited_guarantee) {
    using Type = std::remove_cv_t<TimeSeries>;

    if constexpr (time_series_variable<Type>) {
        return time_series_variable_instance<Scope, Type>();
    } else {
        const auto guarantee = [&] {
            if constexpr (Type::has_explicit_guarantee) {
                return guarantee_parameter<Scope, Type::guarantee>();
            } else {
                return inherited_guarantee;
            }
        }();

        return time_series_type_traits<Type>::template get<Scope, Index, Input>(guarantee);
    }
}

} // namespace bg::details
