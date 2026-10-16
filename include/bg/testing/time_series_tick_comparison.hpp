#pragma once

#include <bg/export.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/util/buffer.hpp>

#include <functional>

namespace bg::testing {

// Register by semantic time-series type. The framework handles invalid ticks before calling this callback.
// The callback borrows and advances reader; on success it consumes exactly one valid payload and leaves borrowed
// values intact.
struct TickLiteralComparison {
    using Compare = std::function<bool(util::buffer_reader &, const TimeSeriesType &,
                                      const literal::TimeSeriesLiteral &)>;

    Compare compare;
};

} // namespace bg::testing

namespace bg {

template <> BG_EXPORT TimeSeriesDispatch<testing::TickLiteralComparison> &
TimeSeriesDispatch<testing::TickLiteralComparison>::instance() noexcept;

} // namespace bg
