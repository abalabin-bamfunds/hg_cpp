#include <hg/time_series/literal_operations.hpp>
#include <hg/util/static_storage.hpp>

namespace hg {

template <> TimeSeriesDispatch<TimeSeriesLiteralOperations> &
TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<TimeSeriesDispatch<TimeSeriesLiteralOperations>>{};
    return dispatch.get();
}

} // namespace hg
