#include <bg/reduce/details/reduce_node_definition.hpp>

#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/scalar_type.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {

ReduceNodeDefinition::ReduceNodeDefinition(ReduceGraphWiring wiring, const ResolvedSignature &signature,
                                           std::uint32_t dictionary_parameter,
                                           std::optional<std::uint32_t> default_parameter)
    : wiring_{std::move(wiring)}, dictionary_parameter_{dictionary_parameter},
      default_parameter_{default_parameter} {
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
        std::optional<GraphWiringStructure> compact_structure;
        std::uint32_t dictionary_parameter;
        std::optional<std::uint32_t> default_parameter;

        std::size_t hash() const noexcept {
            auto result = structure.hash();
            if (compact_structure) {
                result ^= compact_structure->hash() + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
            }
            result ^= dictionary_parameter + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
            result ^= default_parameter.value_or(std::numeric_limits<std::uint32_t>::max()) +
                      0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
            return result;
        }

        bool cacheable() const noexcept {
            return structure.cacheable() && (!compact_structure || compact_structure->cacheable());
        }
        bool operator==(const Identity &) const = default;
    };
    return NodeTemplateIdentity::from_value(
        Identity{graph_wiring_structure(*wiring_.wiring, boundaries, wiring_.output_node),
                 wiring_.compact_graph ? std::optional{wiring_.compact_graph->structure} : std::nullopt,
                 dictionary_parameter_, default_parameter_});
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

    auto compact_graph = std::optional<CompiledReduceGraph::CompactGraph>{};
    if (wiring_.compact_graph) {
        auto compact_node_indices = std::vector<std::size_t>{};
        if (wiring_.compact_graph->output_node) {
            compact_node_indices.push_back(*wiring_.compact_graph->output_node);
        }
        auto graph = compile_graph(*wiring_.compact_graph->wiring, memory, compact_node_indices);
        const auto inputs = graph->runtime_inputs();
        const auto input_indices = wiring_.compact_graph->input_indices;
        const auto has_only_reducer_inputs =
            inputs.size() == input_indices.size() && input_indices[0] != input_indices[1] &&
            input_indices[0] < inputs.size() && input_indices[1] < inputs.size();
        if (graph->supports_borrowed_evaluation() && has_only_reducer_inputs) {
            auto output_node = std::optional<std::size_t>{};
            if (wiring_.compact_graph->output_node) {
                output_node = compact_node_indices.front();
            }
            auto output_input = std::optional<std::size_t>{};
            auto compatible_output = true;
            if (output_node) {
                const auto &output_runtime_type = graph->node(*output_node).node_invocation_output_runtime_type();
                compatible_output = std::ranges::all_of(wiring_.compact_graph->input_indices,
                                                        [&](const auto input_index) {
                                                            return input_index < inputs.size() &&
                                                                   inputs[input_index].runtime_type ==
                                                                       &output_runtime_type;
                                                        });
            } else if (wiring_.compact_graph->output_input) {
                const auto returned_input = *wiring_.compact_graph->output_input;
                if (returned_input == wiring_.compact_graph->input_indices[0]) {
                    output_input = 0;
                } else if (returned_input == wiring_.compact_graph->input_indices[1]) {
                    output_input = 1;
                } else {
                    compatible_output = false;
                }
            } else {
                compatible_output = false;
            }
            if (compatible_output) {
                compact_graph.emplace(CompiledReduceGraph::CompactGraph{
                    .graph = std::move(graph),
                    .input_indices = wiring_.compact_graph->input_indices,
                    .output_node = output_node,
                    .output_input = output_input,
                });
            }
        }
    }

    return {.nested = std::move(nested),
            .compact_graph = std::move(compact_graph),
            .dictionary_parameter = dictionary_parameter_,
            .default_parameter = default_parameter_};
}

void ReduceNodeDefinition::validate() const {
    if (wiring_.wiring == nullptr || wiring_.output_node >= wiring_.wiring->nodes().size()) {
        throw std::invalid_argument{"a reduce node requires a wired binary graph"};
    }
    if (dictionary_parameter_ >= signature_->parameter_count() ||
        (default_parameter_ && *default_parameter_ >= signature_->parameter_count())) {
        throw std::invalid_argument{"a reduce node parameter index is out of range"};
    }
    if (signature_->output() == nullptr || type_cast<TimeSeriesValueType>(signature_->output()) == nullptr) {
        throw std::invalid_argument{"a reduce node output must be a scalar-valued time series"};
    }

    const auto *dictionary = type_cast<TimeSeriesDictionaryType>(signature_->parameter(dictionary_parameter_).type);
    const auto *value_type = dictionary == nullptr
                                 ? nullptr
                                 : type_cast<TimeSeriesValueType>(&dictionary->value_type());
    const auto *output_type = type_cast<TimeSeriesValueType>(signature_->output());
    if (value_type == nullptr || output_type == nullptr ||
        &value_type->value_type() != &output_type->value_type()) {
        throw std::invalid_argument{"a reduce node requires a value TSD input for its output type"};
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
