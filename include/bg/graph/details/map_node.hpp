#pragma once

#include <bg/type/signature.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace bg {

class GraphWiring;
class NodeTemplate;

namespace details {

enum class MapInputKind {
    mapped,
    pass_through,
};

struct MapInputBoundary {
    MapInputKind kind;
    std::size_t parameter_index;
    std::size_t input_node;
};

// Describes an isolated inner graph after its public arguments have been replaced by boundary nodes.
struct MapGraphWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::vector<MapInputBoundary> inputs{};
    std::optional<std::size_t> key_input_node{};
    std::optional<std::size_t> output_node{};
    std::optional<std::size_t> key_set_parameter{};
};

[[nodiscard]] std::unique_ptr<NodeTemplate> make_map_node(MapGraphWiring wiring,
                                                         const ResolvedSignature &outer_signature);

} // namespace details
} // namespace bg
