#pragma once

#include <bg/graph/nested_graph.hpp>
#include <bg/type/signature.hpp>

#include <memory>
#include <vector>

namespace bg {

class NodeTemplate;
class GraphWiring;
class TimeSeriesType;
class TimeSeriesReferenceType;
class WiringPoint;

namespace details {

struct NestedGraphWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::vector<std::size_t> input_nodes;
    std::size_t output_node;
};

[[nodiscard]] WiringPoint wire_nested_input(const TimeSeriesReferenceType &type);
[[nodiscard]] WiringPoint wire_nested_constant_input(const TimeSeriesType &type);
[[nodiscard]] WiringPoint wire_nested_output(WiringPoint output, const TimeSeriesType &type);
[[nodiscard]] WiringPoint wire_validity_filtered_nested_output(WiringPoint output, const TimeSeriesType &type);
// Time-series parameters in graph_signature must already have complete reference types.
[[nodiscard]] std::unique_ptr<NodeTemplate> make_nested_node(NestedGraphWiring wiring,
                                                             const ResolvedSignature &graph_signature);

} // namespace details
} // namespace bg
