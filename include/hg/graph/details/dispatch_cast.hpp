#pragma once

#include <hg/graph/wiring_point.hpp>

namespace hg {

class TimeSeriesType;

namespace details {

// Reinterpret a covariant value or reference type without adding a node.
// The caller must ensure that a narrowed runtime value has the advertised subtype.
WiringPoint dispatch_cast(WiringPoint source, const TimeSeriesType &target_type);

} // namespace details
} // namespace hg
