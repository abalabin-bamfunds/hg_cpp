#pragma once

#include <bg/graph_run_input.hpp>

namespace bg::testing {

using TestGraphLiteralInput = GraphRunLiteralInput;

template <typename TimeSeries>
[[nodiscard]] GraphRunLiteralInput input(GraphRunLiteralInput source) {
    return bg::input<TimeSeries>(std::move(source));
}

[[nodiscard]] inline GraphRunLiteralInput input(const TimeSeriesRuntimeType &runtime_type,
                                                GraphRunLiteralInput source) {
    return bg::input(runtime_type, std::move(source));
}

[[nodiscard]] inline GraphRunLiteralInput input(const TimeSeriesType &time_series_type,
                                                GraphRunLiteralInput source) {
    return bg::input(time_series_type, std::move(source));
}

} // namespace bg::testing
