#include <bg/reduce/details/reduce_node_definition.hpp>

#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
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

ReduceNodeDefinition::ReduceNodeDefinition(ReduceGraphWiring wiring, const ResolvedSignature &signature,
                                           std::uint32_t value_dictionary_parameter,
                                           std::uint32_t reference_dictionary_parameter,
                                           std::optional<std::uint32_t> default_parameter)
    : wiring_{std::move(wiring)}, value_dictionary_parameter_{value_dictionary_parameter},
      reference_dictionary_parameter_{reference_dictionary_parameter}, default_parameter_{default_parameter} {
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(signature.parameters.size());
    for (const auto &parameter : signature.parameters) {
        parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                              parameter.accepts_absent_scalar, details::VariadicMode::none, nullptr, false,
                              parameter.dynamic_activity});
    }
    signature_ = std::make_unique<Signature>(signature.name, std::move(parameters), *signature.output);
    validate();
}

NodeTemplateIdentity ReduceNodeDefinition::definition_identity() const {
    const auto boundaries = std::array{wiring_.input_nodes[0], wiring_.input_nodes[1]};
    struct Identity {
        GraphWiringStructure structure;
        std::uint32_t value_dictionary_parameter;
        std::uint32_t reference_dictionary_parameter;
        std::optional<std::uint32_t> default_parameter;

        std::size_t hash() const noexcept {
            auto result = structure.hash();
            result ^= value_dictionary_parameter + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
            result ^= reference_dictionary_parameter + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
            result ^= default_parameter.value_or(std::numeric_limits<std::uint32_t>::max()) +
                      0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
            return result;
        }

        bool cacheable() const noexcept { return structure.cacheable(); }
        bool operator==(const Identity &) const = default;
    };
    return NodeTemplateIdentity::from_value(
        Identity{graph_wiring_structure(*wiring_.wiring, boundaries, wiring_.output_node),
                 value_dictionary_parameter_, reference_dictionary_parameter_, default_parameter_});
}

ReduceNodeDefinition::compiled_data_type ReduceNodeDefinition::compile(const ResolvedSignature &, allocator &memory) const {
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
            .value_dictionary_parameter = value_dictionary_parameter_,
            .reference_dictionary_parameter = reference_dictionary_parameter_,
            .default_parameter = default_parameter_};
}

void ReduceNodeDefinition::validate() const {
    if (wiring_.wiring == nullptr || wiring_.output_node >= wiring_.wiring->nodes().size()) {
        throw std::invalid_argument{"a reduce node requires a wired binary graph"};
    }
    if (value_dictionary_parameter_ >= signature_->parameter_count() ||
        reference_dictionary_parameter_ >= signature_->parameter_count() ||
        (default_parameter_ && *default_parameter_ >= signature_->parameter_count())) {
        throw std::invalid_argument{"a reduce node parameter index is out of range"};
    }
    if (signature_->output() == nullptr || type_cast<TimeSeriesValueType>(signature_->output()) == nullptr) {
        throw std::invalid_argument{"a reduce node output must be a scalar-valued time series"};
    }

    const auto *value_dictionary =
        type_cast<TimeSeriesDictionaryType>(signature_->parameter(value_dictionary_parameter_).type);
    const auto *reference_dictionary =
        type_cast<TimeSeriesDictionaryType>(signature_->parameter(reference_dictionary_parameter_).type);
    const auto *value_type = value_dictionary == nullptr
                                 ? nullptr
                                 : type_cast<TimeSeriesValueType>(&value_dictionary->value_type());
    const auto *item_reference = reference_dictionary == nullptr
                                     ? nullptr
                                     : type_cast<TimeSeriesReferenceType>(&reference_dictionary->value_type());
    if (value_type == nullptr || item_reference == nullptr ||
        &value_dictionary->key_type() != &reference_dictionary->key_type() ||
        !value_type->accepts(*signature_->output()) || !item_reference->target_type().accepts(*signature_->output())) {
        throw std::invalid_argument{"a reduce node requires value and reference TSD inputs for its output type"};
    }

    if (default_parameter_) {
        const auto *default_type = type_cast<ScalarType>(signature_->parameter(*default_parameter_).type);
        const auto *output_value = type_cast<TimeSeriesValueType>(signature_->output());
        if (default_type == nullptr || output_value == nullptr || !output_value->value_type().accepts(*default_type)) {
            throw std::invalid_argument{"a reduce node default must match its output scalar type"};
        }
    }
}

} // namespace bg::details
