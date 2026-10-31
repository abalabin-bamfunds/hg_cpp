#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace bg {

class GraphWiring;

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

// Describes the inner graph and boundary nodes used for every map entry.
struct MapGraphWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::vector<MapInputBoundary> inputs{};
    std::optional<std::size_t> key_input_node{};
    std::optional<std::size_t> output_node{};
    std::optional<std::size_t> key_set_parameter{};
    bool valid_items{};
};

} // namespace details
} // namespace bg
