#pragma once

#include <bg/map/details/map_graph_wiring.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace bg {

class NodeTemplate;

namespace details {

[[nodiscard]] std::unique_ptr<NodeTemplate> make_map_node(MapGraphWiring wiring,
                                                          const ResolvedSignature &outer_signature);

} // namespace details
} // namespace bg
