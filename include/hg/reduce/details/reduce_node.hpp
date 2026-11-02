#pragma once

#include <hg/reduce/details/reduce_graph_argument.hpp>
#include <hg/type/signature.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace hg {

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
} // namespace hg
