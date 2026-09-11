#pragma once

#include <bg/time_series/item_id.hpp>

namespace bg {

class GraphWiring;
class TimeSeriesType;
class TimeSeriesRuntimeType;
class WiringPoint;
struct WiredOutput;

namespace details {

WiringPoint add_wired_output(GraphWiring &wiring, WiredOutput output);
void set_wired_output_runtime_type(WiringPoint output, const TimeSeriesRuntimeType &runtime_type);
WiringPoint wiring_point_for_item(WiringPoint parent, item_id_t item, const TimeSeriesType &type);

} // namespace details
} // namespace bg
