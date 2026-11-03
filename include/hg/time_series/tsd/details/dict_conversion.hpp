#pragma once

#include <hg/graph/wiring_point.hpp>

namespace hg {

class TimeSeriesDictionaryType;

namespace details {

// Wire a scalar-dictionary snapshot as a TSD with the requested item and collection guarantees.
WiringPoint wire_dict_to_tsd(WiringPoint input, const TimeSeriesDictionaryType &output_type);

} // namespace details
} // namespace hg
