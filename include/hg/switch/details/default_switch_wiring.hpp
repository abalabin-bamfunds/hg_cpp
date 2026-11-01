#pragma once

#include <hg/graph/graph_argument.hpp>
#include <hg/graph/wiring_argument.hpp>
#include <hg/switch/details/switch_node.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace hg::details {

struct PreparedSwitchCase {
    std::vector<WiringArgument> keys;
    bool default_case;
    const WiringTemplate *function;
    bool preserve_argument_names;
    std::shared_ptr<const WiringTemplate> function_owner;
};

struct DefaultSwitchWiring {
    SwitchNodeWiring node;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;

    static const DefaultSwitchWiring &from(const GraphArgument &argument);
    std::optional<WiringPoint> wire() const;
};

} // namespace hg::details
