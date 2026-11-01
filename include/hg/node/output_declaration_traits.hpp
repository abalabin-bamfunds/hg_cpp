#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>

namespace bg {

template <typename TimeSeries, auto... Options> class Out;

// Extension API for selecting the runtime representation of a configured node output.
template <typename Output> struct output_declaration_traits;

template <typename TimeSeries> struct output_declaration_traits<Out<TimeSeries>> {
    using time_series_type = TimeSeries;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &resolved_type) {
        if constexpr (details::generic_time_series<TimeSeries>) {
            return resolve_time_series_runtime_type(resolved_type);
        } else {
            return cpp_time_series_runtime_type<TimeSeries>(resolved_type);
        }
    }
};

} // namespace bg
