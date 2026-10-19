#include <bg/testing/time_series_tick_comparison.hpp>
#include <bg/util/static_storage.hpp>

namespace bg {

template <> TimeSeriesDispatch<testing::TickLiteralComparison> &
TimeSeriesDispatch<testing::TickLiteralComparison>::instance() noexcept {
    static auto dispatch = util::static_storage<TimeSeriesDispatch<testing::TickLiteralComparison>>{};
    return dispatch.get();
}

} // namespace bg
