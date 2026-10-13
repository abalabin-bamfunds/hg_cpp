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
    struct CompactGraph {
        std::shared_ptr<const CompiledGraph> graph;
        std::array<std::size_t, 2> input_indices;
        std::optional<std::size_t> output_node;
        std::optional<std::size_t> output_input;
        const TimeSeriesRuntimeType *output_runtime_type;
    };

    CompiledNestedGraph nested;
    std::optional<CompactGraph> compact_graph;
    std::uint32_t dictionary_parameter{};
    std::optional<std::uint32_t> default_parameter;
};

class ReduceNodeDefinition {
  public:
    using compiled_data_type = CompiledReduceGraph;

    ReduceNodeDefinition(ReduceGraphWiring wiring, const ResolvedSignature &signature,
                         std::uint32_t dictionary_parameter,
                         std::optional<std::uint32_t> default_parameter);

    const Signature &signature() const noexcept { return *signature_; }
    NodeTemplateIdentity definition_identity() const;
    compiled_data_type compile(const ResolvedSignature &, allocator &memory) const;

  private:
    void validate() const;

    ReduceGraphWiring wiring_;
    std::unique_ptr<Signature> signature_;
    std::uint32_t dictionary_parameter_;
    std::optional<std::uint32_t> default_parameter_;
};

} // namespace bg::details
