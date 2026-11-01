#pragma once

#include <bg/map/details/map_graph_argument.hpp>
#include <bg/graph/details/mesh_node.hpp>
#include <bg/graph/graph_argument.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bg::details {

struct DefaultMeshWiring {
    MapGraphWiring graph;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;
    std::optional<std::size_t> dictionary_key_set_parameter;

    static const DefaultMeshWiring &from(const GraphArgument &argument);
    std::optional<WiringPoint> wire() const;
};

GraphArgument build_mesh_graph_argument(const WiringTemplate &function, ResolvedMapCall call,
                                        bool preserve_named_arguments, std::string name,
                                        const TypeMap &type_map);

WiringPoint mesh_dependency_selection(WiringArgument key, std::string_view name = {});

} // namespace bg::details
