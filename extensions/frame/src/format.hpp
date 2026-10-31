#pragma once

#include <bg/scalar/format_operations.hpp>
#include <bg/time_series/tick_format_operations.hpp>

namespace bg::frame::details {

void register_column_format_type(const ScalarType &type);
void register_frame_format_type(const ScalarType &type);
void register_column_tick_format_type(const TimeSeriesType &type);
void register_frame_tick_format_type(const TimeSeriesType &type);

} // namespace bg::frame::details
