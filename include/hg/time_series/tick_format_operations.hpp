#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/export.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/buffer.hpp>

#include <fmt/format.h>

namespace bg {

// Register operations before formatting begins on concurrent threads. The callback receives borrowed input or
// payload data and a format context that remain valid only for the duration of the call. Serialized callbacks start
// at the payload for their type and must advance the reader past that payload. Do not retain any callback argument.
struct TimeSeriesTickFormatOperations {
    using LiveFormat = void (*)(const In<TS_T> &, EvaluationTime, fmt::format_context &);
    using SerializedFormat = void (*)(const TimeSeriesType &, util::buffer_reader &, fmt::format_context &);

    LiveFormat live{};
    SerializedFormat serialized{};
};

template <> BG_EXPORT TimeSeriesDispatch<TimeSeriesTickFormatOperations> &
TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance() noexcept;

} // namespace bg
