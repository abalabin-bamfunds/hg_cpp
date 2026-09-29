#pragma once

#include <bg/graph/nested_graph.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace bg {

class GraphWiring;
class NodeTemplate;

namespace details {

struct SwitchInputBoundary {
    std::size_t parameter_index;
    std::size_t input_node;
};

struct SwitchBranchWiring {
    std::shared_ptr<const GraphWiring> wiring;
    std::vector<SwitchInputBoundary> inputs;
    std::optional<std::size_t> inherited_parameter_start;
    std::optional<std::size_t> output_node;
};

struct SwitchCaseWiring {
    std::vector<std::shared_ptr<const ScalarValue<>>> keys;
    bool default_case;
    SwitchBranchWiring branch;
};

struct SwitchNodeWiring {
    std::vector<SwitchCaseWiring> cases;
};

std::unique_ptr<NodeTemplate> make_switch_node(SwitchNodeWiring wiring, const ResolvedSignature &outer_signature);

} // namespace details
} // namespace bg
