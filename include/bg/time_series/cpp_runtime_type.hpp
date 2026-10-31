#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

template <typename TimeSeries, TimeSeriesTypeRole Role>
const TimeSeriesType &concrete_time_series_type(TimeSeriesGuarantee inherited) {
    return time_series_type_for<TimeSeries, 0, Role, TimeSeries>(inherited);
}

template <typename TimeSeries> struct CppTimeSeriesRuntimeType;

template <typename T, auto... Properties> struct CppTimeSeriesRuntimeType<TS<T, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return time_series_value_runtime_type(type, cpp_scalar_runtime_type<T>());
    }
};

template <typename Target, auto... Properties> struct CppTimeSeriesRuntimeType<REF<Target, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        const auto &reference = static_cast<const TimeSeriesReferenceType &>(type);
        return time_series_reference_runtime_type(reference);
    }
};

} // namespace bg::details

namespace bg {

// Maps a C++ time series declaration to its default physical representation. Providers specialize this trait when
// their declaration has a runtime representation that differs from the composed defaults.
template <typename TimeSeries> struct time_series_runtime_traits {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return details::CppTimeSeriesRuntimeType<TimeSeries>::get(type);
    }
};

template <typename TimeSeries> const TimeSeriesRuntimeType &cpp_time_series_runtime_type(const TimeSeriesType &type) {
    const auto &runtime_type = time_series_runtime_traits<TimeSeries>::get(type);
    if (type.is_resolved()) {
        register_time_series_runtime_type(runtime_type);
    }
    return runtime_type;
}

template <typename TimeSeries, TimeSeriesTypeRole Role = TimeSeriesTypeRole::declaration>
inline const TimeSeriesRuntimeType *cpp_time_series_runtime_type_v = [] {
    const auto &type = details::concrete_time_series_type<TimeSeries, Role>(TimeSeriesGuarantee::ordinary);
    return &cpp_time_series_runtime_type<TimeSeries>(type);
}();

template <typename TimeSeries, TimeSeriesTypeRole Role>
const TimeSeriesRuntimeType &cpp_time_series_runtime_type() {
    return *cpp_time_series_runtime_type_v<TimeSeries, Role>;
}

} // namespace bg
