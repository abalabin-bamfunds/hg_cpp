#pragma once

#include <bg/reduce/details/reduce_graph_argument.hpp>
#include <bg/type/signature.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace bg {

class NodeTemplate;

namespace details {

[[nodiscard]] std::unique_ptr<NodeTemplate> make_reduce_node(ReduceGraphWiring wiring,
                                                              const ResolvedSignature &signature,
                                                              std::uint32_t value_dictionary_parameter,
                                                              std::uint32_t reference_dictionary_parameter,
                                                              std::optional<std::uint32_t> default_parameter);

} // namespace details
} // namespace bg
