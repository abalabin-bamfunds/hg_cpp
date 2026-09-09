#pragma once

#include <bg/time_series/item_id.hpp>

namespace bg {

class GraphWiring;
class TimeSeriesType;
class WiringPoint;
struct WiredOutput;

namespace details {

WiringPoint add_wired_output(GraphWiring &wiring, WiredOutput output);
WiringPoint wiring_point_for_item(WiringPoint parent, TimeSeriesItemId item, const TimeSeriesType &type);

} // namespace details
} // namespace bg
