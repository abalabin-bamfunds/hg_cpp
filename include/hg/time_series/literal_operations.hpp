#pragma once

#include <hg/export.hpp>
#include <hg/literal/time_series_literal.hpp>
#include <hg/time_series/dispatch.hpp>
#include <hg/time_series/output_state.hpp>

namespace hg {

struct TimeSeriesLiteralOperations {
    // The source literal and output state are borrowed for the call. The callback publishes changes through output.
    void (*apply)(const literal::TimeSeriesLiteral &, OutputState &);
};

template <> HG_EXPORT TimeSeriesDispatch<TimeSeriesLiteralOperations> &
TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance() noexcept;

} // namespace hg
