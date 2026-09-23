#pragma once

#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/switch/details/switch_node.hpp>

#include <optional>
#include <vector>

namespace bg::details {

struct PreparedSwitchCase {
    std::vector<WiringArgument> keys;
    bool default_case;
    const WiringTemplate *function;
    bool preserve_argument_names;
};

struct DefaultSwitchWiring {
    SwitchNodeWiring node;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;

    static const DefaultSwitchWiring &from(const GraphArgument &argument);
    std::optional<WiringPoint> wire() const;
};

} // namespace bg::details
