#include <bg/time_series/literal_operations.hpp>
#include <bg/util/static_storage.hpp>

namespace bg {

template <> TimeSeriesDispatch<TimeSeriesLiteralOperations> &
TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<TimeSeriesDispatch<TimeSeriesLiteralOperations>>{};
    return dispatch.get();
}

} // namespace bg
