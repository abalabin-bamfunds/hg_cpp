#pragma once

#include <bg/reduce/details/reduce_graph_argument.hpp>
#include <bg/type/signature.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace bg {

class NodeTemplate;

namespace details {

[[nodiscard]] std::unique_ptr<NodeTemplate> make_nested_reduce_node(ReduceNestedGraphWiring wiring,
                                                                     const ResolvedSignature &signature,
                                                                     std::uint32_t dictionary_parameter,
                                                                     std::optional<std::uint32_t> default_parameter);
[[nodiscard]] std::unique_ptr<NodeTemplate> make_compact_reduce_node(ReduceCompactGraphWiring wiring,
                                                                      const ResolvedSignature &signature,
                                                                      std::uint32_t dictionary_parameter,
                                                                      std::optional<std::uint32_t> default_parameter);

} // namespace details
} // namespace bg
