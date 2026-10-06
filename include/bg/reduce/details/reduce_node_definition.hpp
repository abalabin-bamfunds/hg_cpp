#pragma once

#include <bg/graph/nested_graph.hpp>
#include <bg/reduce/details/reduce_graph_argument.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/signature.hpp>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>

namespace bg::details {

struct CompiledReduceGraph {
    CompiledNestedGraph nested;
    std::uint32_t value_dictionary_parameter{};
    std::uint32_t reference_dictionary_parameter{};
    std::optional<std::uint32_t> default_parameter;
};

class ReduceNodeDefinition {
  public:
    using compiled_data_type = CompiledReduceGraph;

    ReduceNodeDefinition(ReduceGraphWiring wiring, const ResolvedSignature &signature,
                         std::uint32_t value_dictionary_parameter,
                         std::uint32_t reference_dictionary_parameter,
                         std::optional<std::uint32_t> default_parameter);

    const Signature &signature() const noexcept { return *signature_; }
    NodeTemplateIdentity definition_identity() const;
    compiled_data_type compile(const ResolvedSignature &, allocator &memory) const;

  private:
    void validate() const;

    ReduceGraphWiring wiring_;
    std::unique_ptr<Signature> signature_;
    std::uint32_t value_dictionary_parameter_;
    std::uint32_t reference_dictionary_parameter_;
    std::optional<std::uint32_t> default_parameter_;
};

} // namespace bg::details
