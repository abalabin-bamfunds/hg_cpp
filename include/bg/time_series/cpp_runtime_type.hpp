#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

template <typename TimeSeries>
[[nodiscard]] const TimeSeriesType &concrete_time_series_type(TimeSeriesGuarantee inherited) {
    return time_series_type_for<TimeSeries, 0, false, TimeSeries>(inherited);
}

template <typename TimeSeries> struct CppTimeSeriesRuntimeType;

template <typename T, auto... Properties> struct CppTimeSeriesRuntimeType<TS<T, Properties...>> {
    [[nodiscard]] static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return time_series_value_runtime_type(type, cpp_scalar_runtime_type<T>());
    }
};

template <typename Target, auto... Properties> struct CppTimeSeriesRuntimeType<REF<Target, Properties...>> {
    [[nodiscard]] static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        const auto &reference = static_cast<const TimeSeriesReferenceType &>(type);
        return time_series_reference_runtime_type(reference,
                                                  CppTimeSeriesRuntimeType<Target>::get(reference.target_type()));
    }
};

} // namespace bg::details

namespace bg {

template <typename TimeSeries> const TimeSeriesRuntimeType &cpp_time_series_runtime_type(const TimeSeriesType &type) {
    return details::CppTimeSeriesRuntimeType<TimeSeries>::get(type);
}

template <typename TimeSeries>
inline const TimeSeriesRuntimeType *cpp_time_series_runtime_type_v = [] {
    const auto &type = details::concrete_time_series_type<TimeSeries>(TimeSeriesGuarantee::ordinary);
    return &cpp_time_series_runtime_type<TimeSeries>(type);
}();

template <typename TimeSeries> const TimeSeriesRuntimeType &cpp_time_series_runtime_type() {
    return *cpp_time_series_runtime_type_v<TimeSeries>;
}

} // namespace bg
