#include <hg/testing/time_series_tick_comparison.hpp>
#include <hg/util/static_storage.hpp>

namespace hg {

template <> TimeSeriesDispatch<testing::TickLiteralComparison> &
TimeSeriesDispatch<testing::TickLiteralComparison>::instance() noexcept {
    static auto dispatch = util::static_storage<TimeSeriesDispatch<testing::TickLiteralComparison>>{};
    return dispatch.get();
}

} // namespace hg
