#include <bg/reduce/details/reduce_node_definition.hpp>

#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/scalar_type.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

struct ReduceNodeIdentity {
    GraphWiringStructure structure;
    bool compact{};
    std::uint32_t dictionary_parameter{};
    std::optional<std::uint32_t> default_parameter;

    std::size_t hash() const noexcept {
        auto result = structure.hash();
        result ^= static_cast<std::size_t>(compact) + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        result ^= dictionary_parameter + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        result ^= default_parameter.value_or(std::numeric_limits<std::uint32_t>::max()) +
                  0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        return result;
    }

    bool cacheable() const noexcept { return structure.cacheable(); }
    bool operator==(const ReduceNodeIdentity &) const = default;
};

std::unique_ptr<Signature> make_signature(const ResolvedSignature &signature) {
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(signature.parameters.size());
    for (const auto &parameter : signature.parameters) {
        parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                              parameter.accepts_absent_scalar, details::VariadicMode::none, nullptr, false,
                              parameter.dynamic_activity});
    }
    return std::make_unique<Signature>(signature.name, std::move(parameters), *signature.output);
}

void validate_signature(const Signature &signature, std::uint32_t dictionary_parameter,
                        std::optional<std::uint32_t> default_parameter) {
    if (dictionary_parameter >= signature.parameter_count() ||
        (default_parameter && *default_parameter >= signature.parameter_count())) {
        throw std::invalid_argument{"a reduce node parameter index is out of range"};
    }
    if (signature.output() == nullptr || !signature.output()->is_time_series()) {
        throw std::invalid_argument{"a reduce node output must be a time series"};
    }

    const auto *dictionary = type_cast<TimeSeriesDictionaryType>(signature.parameter(dictionary_parameter).type);
    if (dictionary == nullptr) {
        throw std::invalid_argument{"a reduce node requires a TSD input"};
    }

    const auto *value_type = type_cast<TimeSeriesValueType>(&dictionary->value_type());
    const auto *output_type = type_cast<TimeSeriesValueType>(signature.output());
    const auto *output_reference = type_cast<TimeSeriesReferenceType>(signature.output());
    const auto &item_type = dictionary->value_type().target_type().with_guarantee_limit(TimeSeriesGuarantee::ordinary);
    if (value_type != nullptr) {
        if (output_type == nullptr || &value_type->value_type() != &output_type->value_type()) {
            throw std::invalid_argument{"a reduce node requires a scalar output matching the TSD item type"};
        }
    } else if (output_reference == nullptr) {
        throw std::invalid_argument{"a reduce node requires a reference output matching the TSD item type"};
    } else {
        const auto &output_target = output_reference->target_type().with_guarantee_limit(TimeSeriesGuarantee::ordinary);
        if (!item_type.accepts(output_target) || !output_target.accepts(item_type)) {
            throw std::invalid_argument{"a reduce node requires a reference output matching the TSD item type"};
        }
    }

    if (default_parameter) {
        const auto &default_type = *signature.parameter(*default_parameter).type;
        if (value_type != nullptr) {
            const auto *default_scalar_type = type_cast<ScalarType>(&default_type);
            if (default_scalar_type == nullptr || !output_type->value_type().accepts(*default_scalar_type)) {
                throw std::invalid_argument{"a reduce node scalar default must match its output type"};
            }
        } else {
            const auto *default_reference_type = type_cast<TimeSeriesReferenceType>(&default_type);
            if (default_reference_type == nullptr) {
                throw std::invalid_argument{"a reduce node default reference must match its TSD item type"};
            }
            const auto &default_target =
                default_reference_type->target_type().with_guarantee_limit(TimeSeriesGuarantee::ordinary);
            if (!item_type.accepts(default_target) || !default_target.accepts(item_type)) {
                throw std::invalid_argument{"a reduce node default reference must match its TSD item type"};
            }
        }
    }
}

} // namespace

NestedReduceNodeDefinition::NestedReduceNodeDefinition(ReduceNestedGraphWiring wiring,
                                                       const ResolvedSignature &signature,
                                                       std::uint32_t dictionary_parameter,
                                                       std::optional<std::uint32_t> default_parameter)
    : wiring_{std::move(wiring)}, signature_{make_signature(signature)},
      dictionary_parameter_{dictionary_parameter}, default_parameter_{default_parameter} {
    if (wiring_.wiring == nullptr || wiring_.output_node >= wiring_.wiring->nodes().size()) {
        throw std::invalid_argument{"a reduce node requires a wired binary graph"};
    }
    validate_signature(*signature_, dictionary_parameter_, default_parameter_);
}

NodeTemplateIdentity NestedReduceNodeDefinition::definition_identity() const {
    const auto boundaries = std::array{wiring_.input_nodes[0], wiring_.input_nodes[1]};
    return NodeTemplateIdentity::from_value(ReduceNodeIdentity{
        graph_wiring_structure(*wiring_.wiring, boundaries, wiring_.output_node), false, dictionary_parameter_,
        default_parameter_});
}

NestedReduceNodeDefinition::compiled_data_type
NestedReduceNodeDefinition::compile(const ResolvedSignature &, allocator &memory) const {
    auto node_indices = std::array{wiring_.input_nodes[0], wiring_.input_nodes[1], wiring_.output_node};
    auto graph = compile_graph(*wiring_.wiring, memory, node_indices);

    auto nested = CompiledNestedGraph{
        .graph = std::move(graph),
        .input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
        .constant_input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
        .parent_service_binding_indices = std::pmr::vector<std::uint32_t>{&memory.memory_resource()},
        .output_node = node_indices[2],
    };
    nested.input_nodes.assign(node_indices.begin(), node_indices.begin() + 2);
    for (const auto &requirement : wiring_.wiring->service_binding_requirements()) {
        nested.parent_service_binding_indices.push_back(
            static_cast<std::uint32_t>(*requirement.parent_requirement));
    }

    return {.nested = std::move(nested),
            .dictionary_parameter = dictionary_parameter_,
            .default_parameter = default_parameter_,
            .reference_output = type_cast<TimeSeriesReferenceType>(signature_->output()) != nullptr};
}

CompactReduceNodeDefinition::CompactReduceNodeDefinition(ReduceCompactGraphWiring wiring,
                                                         const ResolvedSignature &signature,
                                                         std::uint32_t dictionary_parameter,
                                                         std::optional<std::uint32_t> default_parameter)
    : wiring_{std::move(wiring)}, signature_{make_signature(signature)},
      dictionary_parameter_{dictionary_parameter}, default_parameter_{default_parameter} {
    if (wiring_.graph == nullptr) {
        throw std::invalid_argument{"a compact reduce node requires a compiled reducer graph"};
    }
    validate_signature(*signature_, dictionary_parameter_, default_parameter_);
}

NodeTemplateIdentity CompactReduceNodeDefinition::definition_identity() const {
    return NodeTemplateIdentity::from_value(
        ReduceNodeIdentity{wiring_.structure, true, dictionary_parameter_, default_parameter_});
}

CompactReduceNodeDefinition::compiled_data_type
CompactReduceNodeDefinition::compile(const ResolvedSignature &, allocator &) const {
    return {.graph = wiring_.graph,
            .input_indices = wiring_.input_indices,
            .output_node = wiring_.output_node,
            .output_input = wiring_.output_input,
            .output_runtime_type = wiring_.output_runtime_type,
            .dictionary_parameter = dictionary_parameter_,
            .default_parameter = default_parameter_};
}

} // namespace bg::details
