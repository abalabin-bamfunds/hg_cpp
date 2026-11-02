#pragma once

#include <hg/evaluation_time.hpp>
#include <hg/export.hpp>
#include <hg/time_series/time_series_input.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/util/buffer.hpp>

#include <fmt/format.h>

namespace hg {

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

HG_EXPORT void format_live_tick(const TimeSeriesInputData &input, EvaluationTime now, fmt::format_context &context);
HG_EXPORT void format_serialized_tick(const TimeSeriesType &type, const util::buffer_reader &reader,
                                      fmt::format_context &context);
HG_EXPORT void format_serialized_tick_payload(const TimeSeriesType &type, bool valid,
                                              const util::buffer_reader &reader, fmt::format_context &context);

} // namespace hg

namespace fmt {

template <> struct formatter<hg::TimeSeriesTickView> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"time-series tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const hg::TimeSeriesTickView &value, format_context &context) const {
        hg::format_live_tick(value.input, value.now, context);
        return context.out();
    }
};

template <> struct formatter<hg::SerializedTimeSeriesTickView> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"time-series tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const hg::SerializedTimeSeriesTickView &value, format_context &context) const {
        hg::format_serialized_tick(value.type, value.reader, context);
        return context.out();
    }
};

template <> struct formatter<hg::SerializedTimeSeriesTickPayloadView> {
    constexpr auto parse(format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw format_error{"time-series tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const hg::SerializedTimeSeriesTickPayloadView &value, format_context &context) const {
        hg::format_serialized_tick_payload(value.type, value.valid, value.reader, context);
        return context.out();
    }
};

} // namespace fmt
