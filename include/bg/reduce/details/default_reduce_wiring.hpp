#pragma once

#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/reduce/details/reduce_graph_argument.hpp>
#include <bg/type/signature.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace bg {

class WiringTemplate;

namespace details {

struct DefaultReduceWiring {
    ReduceGraphWiring graph;
    ResolvedSignature outer_signature;
    std::vector<WiringArgument> outer_arguments;
    std::uint32_t dictionary_parameter{};
    std::optional<std::uint32_t> default_parameter;

    static const DefaultReduceWiring &from(const GraphArgument &argument);
    std::optional<WiringPoint> wire() const;
};

GraphArgument build_reduce_graph_argument(const WiringTemplate &function, const WiringArgument &dictionary,
                                         const std::optional<WiringArgument> &default_value,
                                         const TypeMap &type_map);

} // namespace details
} // namespace bg
