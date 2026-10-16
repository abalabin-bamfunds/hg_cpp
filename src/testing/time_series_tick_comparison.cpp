#include <bg/testing/time_series_tick_comparison.hpp>

namespace bg {

template <> TimeSeriesDispatch<testing::TickLiteralComparison> &
TimeSeriesDispatch<testing::TickLiteralComparison>::instance() noexcept {
    static auto dispatch = TimeSeriesDispatch<testing::TickLiteralComparison>{};
    return dispatch;
}

} // namespace bg
