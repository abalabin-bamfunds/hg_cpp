#pragma once

#include <hg/graph/wiring_argument.hpp>
#include <hg/map/details/map_graph_wiring.hpp>
#include <hg/type/signature.hpp>

#include <cstddef>
#include <memory>

namespace hg {

class NodeTemplate;
class TimeSeriesType;

namespace details {

WiringPoint wire_mesh_dependency(const WiringArgument &key, std::size_t mesh_depth, const TimeSeriesType &target_type);

std::unique_ptr<NodeTemplate> make_mesh_node(MapGraphWiring wiring, const ResolvedSignature &outer_signature);

} // namespace details
} // namespace hg
