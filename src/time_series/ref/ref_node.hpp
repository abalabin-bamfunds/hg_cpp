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
[[nodiscard]] WiringPoint materialize_inherited_reference(WiringPoint argument);
[[nodiscard]] WiringPoint materialize_late_inherited_reference(WiringPoint argument,
                                                               const TimeSeriesType &transport_type);
[[nodiscard]] WiringPoint forward_inherited_reference(WiringPoint source, const TimeSeriesReferenceType &type);

} // namespace details
} // namespace bg
