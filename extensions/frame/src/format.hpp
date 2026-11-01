#pragma once

#include <hg/scalar/format_operations.hpp>
#include <hg/time_series/tick_format_operations.hpp>

namespace hg::frame::details {

void register_column_format_type(const ScalarType &type);
void register_frame_format_type(const ScalarType &type);
void register_column_tick_format_type(const TimeSeriesType &type);
void register_frame_tick_format_type(const TimeSeriesType &type);

} // namespace hg::frame::details
