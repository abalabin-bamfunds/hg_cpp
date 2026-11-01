#pragma once

#include <hg/graph/nested_graph.hpp>
#include <hg/map/details/map_graph_wiring.hpp>
#include <hg/node/node_template.hpp>
#include <hg/type/signature.hpp>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>

namespace hg::details {

struct CompiledMapInput {
    MapInputKind kind;
    std::uint32_t parameter_index;
    std::uint32_t inner_input_index;
};

struct CompiledMapGraph {
    CompiledNestedGraph nested;
    std::pmr::vector<CompiledMapInput> inputs;
    std::optional<std::uint32_t> key_set_parameter{};
    bool valid_items{};
};

// Owns the common signature, identity, validation, and compilation rules for nodes that run one inner graph per key.
class MapNodeDefinition {
  public:
    using compiled_data_type = CompiledMapGraph;

    MapNodeDefinition(MapGraphWiring wiring, const ResolvedSignature &outer_signature);

    const Signature &signature() const noexcept { return *signature_; }
    NodeTemplateIdentity definition_identity() const;
    compiled_data_type compile(const ResolvedSignature &, allocator &memory) const;

  protected:
    const MapGraphWiring &wiring() const noexcept { return wiring_; }

  private:
    void validate() const;

    MapGraphWiring wiring_;
    std::unique_ptr<Signature> signature_;
};

} // namespace hg::details
