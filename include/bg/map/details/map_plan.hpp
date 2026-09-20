#pragma once

#include <bg/graph/details/map_node.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace bg {

class ScalarType;
class TimeSeriesType;
class WiringTemplate;

namespace details {

class MapArgumentSelector;

struct MapSuppliedArgument {
    WiringArgument value;
    bool pass_through{};
};

struct MapPlan {
    MapGraphWiring graph;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;
    const TimeSeriesType *output_type{};
    std::optional<std::size_t> dictionary_controller_parameter;
};

std::optional<std::size_t>
selected_map_argument_index(const std::optional<MapArgumentSelector> &selector,
                            const std::vector<MapSuppliedArgument> &positional,
                            const std::vector<MapSuppliedArgument> &named);

GraphArgument build_map_plan(const WiringTemplate &function,
                             std::vector<MapSuppliedArgument> positional,
                             std::vector<MapSuppliedArgument> named,
                             const ScalarType &key_type,
                             const std::optional<MapArgumentSelector> &key_argument,
                             const std::optional<MapArgumentSelector> &key_set_argument,
                             bool preserve_named_arguments = true);

const MapPlan &map_plan(const GraphArgument &argument);
std::optional<WiringPoint> wire_map_plan(const GraphArgument &argument);

} // namespace details
} // namespace bg
