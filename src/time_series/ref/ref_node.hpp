#pragma once

namespace bg {

class NodeTemplate;
class TimeSeriesReferenceType;
class TimeSeriesType;
class WiringPoint;

namespace details {

[[nodiscard]] const NodeTemplate &reference_node_template(const TimeSeriesType &source,
                                                          const TimeSeriesReferenceType &type);
[[nodiscard]] WiringPoint materialize_reference(WiringPoint source, const TimeSeriesType &requested_type);

} // namespace details
} // namespace bg
