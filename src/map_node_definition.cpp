#include <bg/map/details/map_node_definition.hpp>

#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

struct MapNodeIdentity {
    GraphWiringStructure graph;
    std::vector<std::size_t> input_metadata;
    std::optional<std::size_t> key_set_parameter;

    std::size_t hash() const noexcept {
        auto result = graph.hash();
        const auto combine = [&](std::size_t value) {
            result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        };
        for (const auto value : input_metadata) {
            combine(value);
        }
        combine(key_set_parameter.value_or(std::numeric_limits<std::size_t>::max()));
        return result;
    }

    bool cacheable() const noexcept { return graph.cacheable(); }

    bool operator==(const MapNodeIdentity &) const = default;
};

} // namespace

MapNodeDefinition::MapNodeDefinition(MapGraphWiring wiring, const ResolvedSignature &outer_signature)
    : wiring_{std::move(wiring)} {
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(outer_signature.parameters.size());
    for (const auto &parameter : outer_signature.parameters) {
        if (!parameter.type->is_time_series()) {
            throw std::invalid_argument{"a map or mesh node accepts only time-series parameters"};
        }
        parameters.push_back(
            {parameter.name, parameter.type, parameter.activity, parameter.validity, parameter.accepts_absent_scalar});
    }

    if (outer_signature.output == nullptr) {
        signature_ = std::make_unique<Signature>(outer_signature.name, std::move(parameters));
    } else {
        signature_ = std::make_unique<Signature>(outer_signature.name, std::move(parameters), *outer_signature.output);
    }
    validate();
}

NodeTemplateIdentity MapNodeDefinition::definition_identity() const {
    auto boundaries = std::vector<std::size_t>{};
    boundaries.reserve(wiring_.inputs.size() + static_cast<std::size_t>(wiring_.key_input_node.has_value()));
    for (const auto &input : wiring_.inputs) {
        boundaries.push_back(input.input_node);
    }
    if (wiring_.key_input_node) {
        boundaries.push_back(*wiring_.key_input_node);
    }

    auto input_metadata = std::vector<std::size_t>{};
    input_metadata.reserve(wiring_.inputs.size());
    for (const auto &input : wiring_.inputs) {
        input_metadata.push_back(input.parameter_index * 2 + static_cast<std::size_t>(input.kind));
    }
    return NodeTemplateIdentity::from_value(MapNodeIdentity{
        .graph = graph_wiring_structure(*wiring_.wiring, boundaries, wiring_.output_node),
        .input_metadata = std::move(input_metadata),
        .key_set_parameter = wiring_.key_set_parameter,
    });
}

MapNodeDefinition::compiled_data_type MapNodeDefinition::compile(const ResolvedSignature &, allocator &memory) const {
    auto node_indices = std::vector<std::size_t>{};
    node_indices.reserve(wiring_.inputs.size() + static_cast<std::size_t>(wiring_.key_input_node.has_value()) +
                         static_cast<std::size_t>(wiring_.output_node.has_value()));
    for (const auto &input : wiring_.inputs) {
        node_indices.push_back(input.input_node);
    }
    const auto key_position = node_indices.size();
    if (wiring_.key_input_node) {
        node_indices.push_back(*wiring_.key_input_node);
    }
    const auto output_position = node_indices.size();
    if (wiring_.output_node) {
        node_indices.push_back(*wiring_.output_node);
    }

    auto graph = compile_graph(*wiring_.wiring, memory, node_indices);
    auto result = compiled_data_type{
        .nested = {.graph = std::move(graph),
                   .input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
                   .constant_input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
                   .parent_service_binding_indices = std::pmr::vector<std::uint32_t>{&memory.memory_resource()},
                   .output_node = wiring_.output_node ? std::optional{node_indices[output_position]} : std::nullopt},
        .inputs = std::pmr::vector<CompiledMapInput>{&memory.memory_resource()},
    };
    result.nested.input_nodes.reserve(wiring_.inputs.size());
    result.inputs.reserve(wiring_.inputs.size());

    for (auto index = std::size_t{}; index < wiring_.inputs.size(); ++index) {
        const auto &input = wiring_.inputs[index];
        result.nested.input_nodes.push_back(node_indices[index]);
        result.inputs.push_back(
            {input.kind, static_cast<std::uint32_t>(input.parameter_index), static_cast<std::uint32_t>(index)});
    }
    if (wiring_.key_input_node) {
        result.nested.constant_input_nodes.push_back(node_indices[key_position]);
    }
    for (const auto &requirement : wiring_.wiring->service_binding_requirements()) {
        result.nested.parent_service_binding_indices.push_back(
            static_cast<std::uint32_t>(*requirement.parent_requirement));
    }
    if (wiring_.key_set_parameter) {
        result.key_set_parameter = static_cast<std::uint32_t>(*wiring_.key_set_parameter);
    }
    return result;
}

void MapNodeDefinition::validate() const {
    if (wiring_.wiring == nullptr) {
        throw std::invalid_argument{"a map or mesh node requires inner graph wiring"};
    }
    if (wiring_.inputs.empty() && !wiring_.key_set_parameter) {
        throw std::invalid_argument{"a map or mesh node requires a mapped input or an explicit key set"};
    }
    if (signature_->parameter_count() > std::numeric_limits<std::uint32_t>::max() ||
        wiring_.inputs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument{"a map or mesh node has too many parameters"};
    }

    const auto nodes = wiring_.wiring->nodes();
    const auto boundary_output = [&](std::size_t node_index) -> const TimeSeriesType & {
        if (node_index >= nodes.size() || nodes[node_index].signature().output == nullptr) {
            throw std::invalid_argument{"an inner graph boundary node must have an output"};
        }
        return *nodes[node_index].signature().output;
    };
    const auto *key_type = static_cast<const ScalarType *>(nullptr);
    const auto require_key_type = [&](const ScalarType &candidate) {
        if (key_type != nullptr && key_type != &candidate) {
            throw std::invalid_argument{"map or mesh key types must match"};
        }
        key_type = &candidate;
    };

    for (const auto &input : wiring_.inputs) {
        if (input.parameter_index >= signature_->parameter_count()) {
            throw std::invalid_argument{"a map or mesh input parameter index is out of range"};
        }
        const auto &type = *signature_->parameter(input.parameter_index).type;
        const auto *boundary_reference = type_cast<TimeSeriesReferenceType>(&boundary_output(input.input_node));
        if (input.kind == MapInputKind::mapped) {
            const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type);
            const auto *item_reference =
                dictionary == nullptr ? nullptr : type_cast<TimeSeriesReferenceType>(&dictionary->value_type());
            if (item_reference == nullptr) {
                throw std::invalid_argument{"a mapped input must be a TSD with reference items"};
            }
            if (boundary_reference == nullptr ||
                !boundary_reference->target_type().accepts(item_reference->target_type())) {
                throw std::invalid_argument{"a mapped boundary type must accept its TSD item target"};
            }
            require_key_type(dictionary->key_type());
            continue;
        }

        const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
        if (reference == nullptr) {
            throw std::invalid_argument{"a pass-through input must be a reference"};
        }
        if (boundary_reference == nullptr || !boundary_reference->target_type().accepts(reference->target_type())) {
            throw std::invalid_argument{"a pass-through boundary type must accept its reference target"};
        }
    }
    if (wiring_.key_set_parameter) {
        if (*wiring_.key_set_parameter >= signature_->parameter_count()) {
            throw std::invalid_argument{"the key-set parameter index is out of range"};
        }
        const auto &type = *signature_->parameter(*wiring_.key_set_parameter).type;
        if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
            require_key_type(set->item_type());
        } else if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
            require_key_type(dictionary->key_type());
        } else {
            throw std::invalid_argument{"a key-set parameter must be a TSS or TSD"};
        }
    }
    if (wiring_.key_input_node && !wiring_.key_set_parameter && wiring_.inputs.empty()) {
        throw std::invalid_argument{"a map or mesh node with a key input requires a source of keys"};
    }
    if (wiring_.output_node.has_value() != signature_->has_output()) {
        throw std::invalid_argument{"the inner graph boundary output does not match its signature"};
    }
    if (wiring_.output_node) {
        const auto *dictionary = type_cast<TimeSeriesDictionaryType>(signature_->output());
        const auto *item_reference =
            dictionary == nullptr ? nullptr : type_cast<TimeSeriesReferenceType>(&dictionary->value_type());
        const auto *boundary_reference = type_cast<TimeSeriesReferenceType>(&boundary_output(*wiring_.output_node));
        if (item_reference == nullptr) {
            throw std::invalid_argument{"a map or mesh output must be a TSD with reference items"};
        }
        if (boundary_reference == nullptr ||
            !item_reference->target_type().accepts(boundary_reference->target_type())) {
            throw std::invalid_argument{"the output item type must accept its boundary target"};
        }
        require_key_type(dictionary->key_type());
    }
    if (wiring_.key_input_node) {
        const auto *key = type_cast<TimeSeriesValueType>(&boundary_output(*wiring_.key_input_node));
        if (key == nullptr || key->guarantee().value() != TimeSeriesGuarantee::const_) {
            throw std::invalid_argument{"a key boundary must be a const time-series value"};
        }
        require_key_type(key->value_type());
    }
}

} // namespace bg::details
