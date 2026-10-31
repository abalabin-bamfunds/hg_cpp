#pragma once

#include <bg/export.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/util/buffer.hpp>

#include <cstddef>
#include <functional>
#include <memory>

namespace bg::testing {

// Owns comparison state for one nested time-series lifetime in one equals() call.
struct BG_EXPORT TickLiteralComparisonState {
    virtual ~TickLiteralComparisonState() = default;

    // Compare one valid payload. tick_index is the output cycle index, including cycles without a tick.
    virtual bool compare(util::buffer_reader &reader, const TimeSeriesType &type,
                         const literal::TimeSeriesLiteral &expected, std::size_t tick_index) = 0;

    // Report an invalid tick. BG has already checked that expected is INVALID.
    virtual void invalidate(std::size_t) {}

    // Check the accumulated comparison when this nested time-series lifetime ends.
    virtual bool finish() = 0;
};

// Register by semantic time-series type. Set create_state to compare successive ticks with state owned by one
// equals() call and one nested time-series lifetime. Otherwise compare is called once per valid tick. The callback
// borrows and advances reader; on success it consumes exactly one valid payload and leaves borrowed values intact.
struct TickLiteralComparison {
    using Compare = std::function<bool(util::buffer_reader &, const TimeSeriesType &,
                                      const literal::TimeSeriesLiteral &)>;

    using CreateState = std::function<std::unique_ptr<TickLiteralComparisonState>()>;

    Compare compare;
    CreateState create_state;
};

} // namespace bg::testing

namespace bg {

template <> BG_EXPORT TimeSeriesDispatch<testing::TickLiteralComparison> &
TimeSeriesDispatch<testing::TickLiteralComparison>::instance() noexcept;

} // namespace bg
