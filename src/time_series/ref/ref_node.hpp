#pragma once

namespace bg {

class NodeTemplate;
class TimeSeriesReferenceType;
class TimeSeriesType;
class WiringPoint;

namespace details {

[[nodiscard]] const NodeTemplate &reference_node_template(const TimeSeriesType &argument,
                                                          const TimeSeriesReferenceType &type);
[[nodiscard]] WiringPoint materialize_reference(WiringPoint argument, const TimeSeriesType &requested_type);

} // namespace details
} // namespace bg
