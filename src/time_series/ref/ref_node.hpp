#pragma once

namespace bg {

class NodeTemplate;
class TimeSeriesReferenceType;
class TimeSeriesType;

namespace details {

[[nodiscard]] const NodeTemplate &reference_node_template(const TimeSeriesType &source,
                                                          const TimeSeriesReferenceType &type);

} // namespace details
} // namespace bg
