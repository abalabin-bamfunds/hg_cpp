#include <bg/time_series/literal_operations.hpp>

namespace bg {

template <> TimeSeriesDispatch<TimeSeriesLiteralOperations> &
TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance() noexcept {
    static auto dispatch = TimeSeriesDispatch<TimeSeriesLiteralOperations>{};
    return dispatch;
}

} // namespace bg
