#pragma once

#include <bg/graph/details/graph_wiring_structure.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

namespace bg {

class GraphWiring;
class TimeSeriesType;
class WiringTemplate;
class TypeMap;

namespace details {

struct ReduceGraphWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::array<std::size_t, 2> input_nodes;
    std::size_t output_node;
    const TimeSeriesType *output_type;
};

struct ReduceGraphArgumentWiring {
    GraphWiringStructure structure;
    ReduceGraphWiring graph;
};

ReduceGraphArgumentWiring build_reduce_graph_argument_wiring(const WiringTemplate &function,
                                                              const TimeSeriesType &item_type,
                                                              const TypeMap &type_map);

} // namespace details
} // namespace bg
