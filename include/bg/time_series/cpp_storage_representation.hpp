#pragma once

#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

template <typename TimeSeries>
[[nodiscard]] const TimeSeriesType &concrete_time_series_type(TimeSeriesGuarantee inherited) {
    return time_series_type_for<TimeSeries, 0, false, TimeSeries>(inherited);
}

template <typename TimeSeries> struct CppTimeSeriesStorageRepresentation;

template <typename T, auto... Properties> struct CppTimeSeriesStorageRepresentation<TS<T, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        return time_series_value_storage_representation(type, cpp_scalar_storage_representation<T>());
    }
};

template <typename Target, auto... Properties> struct CppTimeSeriesStorageRepresentation<REF<Target, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        const auto &reference = static_cast<const TimeSeriesReferenceType &>(type);
        return time_series_reference_storage_representation(
            reference, CppTimeSeriesStorageRepresentation<Target>::get(reference.target_type()));
    }
};

} // namespace bg::details

namespace bg {

template <typename TimeSeries>
const TimeSeriesStorageRepresentation &cpp_time_series_storage_representation(const TimeSeriesType &type) {
    return details::CppTimeSeriesStorageRepresentation<TimeSeries>::get(type);
}

template <typename TimeSeries> const TimeSeriesStorageRepresentation &cpp_time_series_storage_representation() {
    const auto &type = details::concrete_time_series_type<TimeSeries>(TimeSeriesGuarantee::ordinary);
    return cpp_time_series_storage_representation<TimeSeries>(type);
}

} // namespace bg
