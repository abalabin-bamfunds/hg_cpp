#pragma once

#include <bg/graph/details/graph_wiring_structure.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

namespace bg {

class GraphWiring;
class CompiledGraph;
class TimeSeriesType;
class TimeSeriesRuntimeType;
class WiringTemplate;
class TypeMap;

namespace details {

struct ReduceCompactGraphWiring {
    std::shared_ptr<const CompiledGraph> graph;
    std::array<std::size_t, 2> input_indices;
    std::optional<std::size_t> output_node;
    std::optional<std::size_t> output_input;
    const TimeSeriesRuntimeType *output_runtime_type;
    GraphWiringStructure structure;
};

struct ReduceNestedGraphWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::array<std::size_t, 2> input_nodes;
    std::size_t output_node;
};

struct ReduceGraphWiring {
    ReduceNestedGraphWiring nested;
    std::optional<ReduceCompactGraphWiring> compact_graph;
};

struct ReduceGraphArgumentWiring {
    GraphWiringStructure structure;
    ReduceGraphWiring graph;
};

ReduceGraphArgumentWiring build_reduce_graph_argument_wiring(const WiringTemplate &function,
                                                              const TimeSeriesType &item_type,
                                                              const TypeMap &type_map,
                                                              bool item_runtime_type_is_fixed = true);

} // namespace details
} // namespace bg
