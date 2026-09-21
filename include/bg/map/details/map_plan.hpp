#pragma once

#include <bg/graph/details/map_node.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/map_argument.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace bg {

class TimeSeriesType;
class WiringTemplate;

namespace details {

class MapArgumentSelector;

struct MapPlan {
    MapGraphWiring graph;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;
    std::optional<std::size_t> dictionary_key_set_parameter;

    static const MapPlan &from(const GraphArgument &argument);
    std::optional<WiringPoint> wire() const;
};

std::optional<std::size_t>
selected_map_argument_index(const std::optional<MapArgumentSelector> &selector,
                            const std::vector<MapArgument> &positional,
                            const std::vector<MapArgument> &named);

GraphArgument build_map_plan(const WiringTemplate &function,
                             std::vector<MapArgument> positional,
                             std::vector<MapArgument> named,
                             const std::optional<MapArgumentSelector> &key_argument,
                             const std::optional<MapArgumentSelector> &key_set_argument,
                             bool preserve_named_arguments = true);

} // namespace details
} // namespace bg
