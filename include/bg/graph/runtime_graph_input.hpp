#pragma once

#include <bg/time_series/time_series_state.hpp>

namespace bg {

class TimeSeriesRuntimeType;

enum class RuntimeGraphInputChange : unsigned char {
    unchanged,
    update,
    invalidation,
    revalidation,
};

struct RuntimeGraphInput {
    const TimeSeriesState *last_updated;
    const void *value;
    const TimeSeriesRuntimeType *runtime_type;
    RuntimeGraphInputChange change;
};

} // namespace bg
