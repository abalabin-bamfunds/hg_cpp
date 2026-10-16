#pragma once

#include <bg/export.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/time_series/output_state.hpp>

namespace bg {

struct TimeSeriesLiteralOperations {
    // The source literal and output state are borrowed for the call. The callback publishes changes through output.
    void (*apply)(const literal::TimeSeriesLiteral &, OutputState &);
};

template <> BG_EXPORT TimeSeriesDispatch<TimeSeriesLiteralOperations> &
TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance() noexcept;

} // namespace bg
