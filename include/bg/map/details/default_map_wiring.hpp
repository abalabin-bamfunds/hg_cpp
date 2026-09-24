#pragma once

#include <bg/map/details/map_graph_argument.hpp>
#include <bg/map/details/map_node.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/map/details/map_call.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace bg {

class WiringTemplate;

namespace details {

struct DefaultMapWiring {
    MapGraphWiring graph;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;
    std::optional<std::size_t> dictionary_key_set_parameter;

    static const DefaultMapWiring &from(const GraphArgument &argument);
    std::optional<WiringPoint> wire() const;
};

GraphArgument build_map_graph_argument(const WiringTemplate &function, ResolvedMapCall call,
                                       bool preserve_named_arguments = true);

} // namespace details
} // namespace bg
