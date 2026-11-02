#pragma once

#include <hg/time_series/time_series_runtime_type.hpp>

namespace hg::frame::details {

template <typename T> void register_column_series_queue_runtime(const TimeSeriesRuntimeType &runtime);
void register_frame_series_queue_runtime(const TimeSeriesRuntimeType &runtime);

} // namespace hg::frame::details
