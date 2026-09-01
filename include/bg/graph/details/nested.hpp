#pragma once

#include <bg/graph/nested_graph.hpp>
#include <bg/type/signature.hpp>

#include <memory>
#include <vector>

namespace bg {

class NodeTemplate;
class GraphWiring;
class TimeSeriesType;
class WiringPoint;

namespace details {

struct NestedGraphWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::vector<std::size_t> input_nodes;
    std::size_t output_node;
};

[[nodiscard]] WiringPoint wire_nested_input(const TimeSeriesType &type);
[[nodiscard]] WiringPoint wire_nested_output(WiringPoint output);
[[nodiscard]] std::unique_ptr<NodeTemplate> make_nested_node(NestedGraphWiring wiring,
                                                             const ResolvedSignature &graph_signature);

} // namespace details
} // namespace bg
