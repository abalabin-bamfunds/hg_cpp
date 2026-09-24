#pragma once

#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/map/details/map_call.hpp>
#include <bg/map/details/map_graph_wiring.hpp>
#include <bg/type/signature.hpp>

#include <optional>
#include <vector>

namespace bg {

class WiringTemplate;

namespace details {

struct MapGraphArgumentWiring {
    GraphWiringStructure structure;
    MapGraphWiring graph;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;
    std::optional<std::size_t> dictionary_key_set_parameter;
};

MapGraphArgumentWiring build_map_graph_argument_wiring(util::symbol name, const WiringTemplate &function,
                                                       ResolvedMapCall call, bool preserve_named_arguments = true);

} // namespace details
} // namespace bg
