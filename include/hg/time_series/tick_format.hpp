#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/export.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/buffer.hpp>

#include <fmt/format.h>

namespace bg {

// Formats a live input at `now`, including root-level SKIP filtering.
struct TimeSeriesTickView {
    TimeSeriesInputData input;
    EvaluationTime now;
};

template <typename Variable, auto... Properties>
[[nodiscard]] auto tick(const In<Variable, Properties...> &input, EvaluationTime now) noexcept {
    return TimeSeriesTickView{.input = input.input_data(), .now = now};
}

struct SerializedTimeSeriesTickView {
    const TimeSeriesType &type;
    const util::buffer_reader &reader;
};

struct SerializedTimeSeriesTickPayloadView {
    const TimeSeriesType &type;
    bool valid;
    const util::buffer_reader &reader;
};

// The reader must point to one complete retained tick record. Formatting forks it and leaves the supplied reader
// and encoded scalar payloads unchanged.
[[nodiscard]] inline SerializedTimeSeriesTickView tick(const TimeSeriesType &type,
                                                       const util::buffer_reader &reader) noexcept {
    return {.type = type, .reader = reader};
}

// The reader must point to the payload after the root tick header has already been read. Formatting forks it and
// leaves the supplied reader unchanged.
[[nodiscard]] inline SerializedTimeSeriesTickPayloadView tick(const TimeSeriesType &type, bool valid,
                                                              const util::buffer_reader &reader) noexcept {
    return {.type = type, .valid = valid, .reader = reader};
}

BG_EXPORT void format_live_tick(const TimeSeriesInputData &input, EvaluationTime now, fmt::format_context &context);
BG_EXPORT void format_serialized_tick(const TimeSeriesType &type, const util::buffer_reader &reader,
                                      fmt::format_context &context);
BG_EXPORT void format_serialized_tick_payload(const TimeSeriesType &type, bool valid,
                                              const util::buffer_reader &reader, fmt::format_context &context);

} // namespace bg

namespace fmt {

template <> struct formatter<bg::TimeSeriesTickView> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"time-series tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const bg::TimeSeriesTickView &value, format_context &context) const {
        bg::format_live_tick(value.input, value.now, context);
        return context.out();
    }
};

template <> struct formatter<bg::SerializedTimeSeriesTickView> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"time-series tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const bg::SerializedTimeSeriesTickView &value, format_context &context) const {
        bg::format_serialized_tick(value.type, value.reader, context);
        return context.out();
    }
};

template <> struct formatter<bg::SerializedTimeSeriesTickPayloadView> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"time-series tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const bg::SerializedTimeSeriesTickPayloadView &value, format_context &context) const {
        bg::format_serialized_tick_payload(value.type, value.valid, value.reader, context);
        return context.out();
    }
};

} // namespace fmt
