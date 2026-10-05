#include <bg/time_series/tick_serializer.hpp>

namespace bg {

TimeSeriesTickDispatch &TimeSeriesTickDispatch::instance() noexcept {
    static auto dispatch = TimeSeriesTickDispatch{};
    return dispatch;
}

} // namespace bg
