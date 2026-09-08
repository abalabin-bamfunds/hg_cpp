#pragma once

#include <span>

namespace bg {

struct ResolvedSignature;
class ScalarRuntimeType;
class TimeSeriesRuntimeType;

namespace details {

const TimeSeriesRuntimeType &
resolve_generic_output_runtime_type(const ResolvedSignature &signature,
                                    std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                                    std::span<const TimeSeriesRuntimeType *const> time_series_input_runtime_types);

} // namespace details
} // namespace bg
