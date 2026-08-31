#pragma once

namespace bg {

class NodeTemplate;
class TimeSeriesReferenceType;

namespace details {

[[nodiscard]] const NodeTemplate &reference_node_template(const TimeSeriesReferenceType &type);

} // namespace details
} // namespace bg
