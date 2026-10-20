#include <bg/reduce/details/reduce_graph_argument.hpp>

#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/service_instance_registry.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <array>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

const TimeSeriesType &reference_target(const TimeSeriesType &type) {
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&type)) {
        return reference->target_type();
    }
    return type;
}

void validate_signature(const WiringTemplate &function) {
    const auto &signature = function.signature();
    if (signature.has_variadic_parameters()) {
        throw WiringError{"a reduce graph must have two fixed time-series inputs"};
    }

    auto explicit_inputs = std::size_t{};
    for (std::size_t index = 0; index < signature.parameter_count(); ++index) {
        const auto &parameter = signature.parameter(index);
        if (parameter.context) {
            continue;
        }
        ++explicit_inputs;
        if (!parameter.type->is_time_series()) {
            throw WiringError{"a reduce graph input must be a time series"};
        }
    }
    if (explicit_inputs != 2) {
        throw WiringError{"a reduce graph must have exactly two explicit inputs"};
    }
    if (signature.output() == nullptr || type_cast<TimeSeriesValueType>(signature.output()) == nullptr) {
        throw WiringError{"a reduce graph output must have the TSD item value type"};
    }
}

bool accepts_compact_inputs(const WiringTemplate &function, const TimeSeriesType &input_type) {
    const auto &signature = function.signature();
    for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
        const auto &parameter = signature.parameter(index);
        if (parameter.context) {
            continue;
        }
        if (type_cast<TimeSeriesValueType>(parameter.type) == nullptr || !parameter.type->accepts(input_type)) {
            return false;
        }
    }
    return true;
}

} // namespace

ReduceGraphArgumentWiring build_reduce_graph_argument_wiring(const WiringTemplate &function,
                                                              const TimeSeriesType &item_type,
                                                              const TypeMap &type_map,
                                                              bool item_runtime_type_is_fixed) {
    const auto *item_value_type = type_cast<TimeSeriesValueType>(&item_type);
    if (item_value_type == nullptr) {
        throw WiringError{"reduce requires TSD items to be scalar-valued time series"};
    }
    validate_signature(function);

    const auto &reducer_input_type = item_value_type->with_guarantee(TimeSeriesGuarantee::ordinary);
    auto wiring = std::make_shared<GraphWiring>(WiringContext::current().wiring());
    auto context = WiringContext{*wiring};

    const auto &reducer_input_reference =
        time_series_reference_type(reducer_input_type, TimeSeriesGuarantee::ordinary);
    auto boundaries = std::array{wire_nested_input(reducer_input_reference),
                                 wire_nested_input(reducer_input_reference)};
    auto arguments = std::array{WiringArgument{boundaries[0]}, WiringArgument{boundaries[1]}};
    const auto result = function(type_map, std::span<const WiringArgument>{arguments});
    if (!result) {
        throw WiringError{"a reduce graph must produce an output"};
    }
    const auto &result_type = reference_target(result->type());
    const auto *result_value_type = type_cast<TimeSeriesValueType>(&result_type);
    if (result_value_type == nullptr || &result_value_type->value_type() != &item_value_type->value_type()) {
        throw WiringError{"a reduce graph output must have the TSD item value type"};
    }

    const auto concrete_result = wiring->complete_pending_wiring(*result);
    const auto structure = graph_wiring_structure(*wiring, boundaries, concrete_result);

    auto compact_graph = std::optional<ReduceCompactGraphWiring>{};
    const auto reducer_input_types = std::array<const TimeSeriesType *, 2>{&reducer_input_type, &reducer_input_type};
    if (item_runtime_type_is_fixed && wiring->inherited_requirements().empty() &&
        wiring->service_binding_requirements().empty()) {
        if (accepts_compact_inputs(function, reducer_input_type)) {
            try {
                auto compact_wiring = std::make_shared<GraphWiring>();
                auto compact_context = WiringContext{*compact_wiring};
                auto compact_inputs = std::array{compact_wiring->borrowed_input(reducer_input_type),
                                                 compact_wiring->borrowed_input(reducer_input_type)};
                auto compact_arguments =
                    std::array{WiringArgument{compact_inputs[0]}, WiringArgument{compact_inputs[1]}};
                const auto compact_result = function(type_map, std::span<const WiringArgument>{compact_arguments});
                if (compact_result) {
                    const auto &compact_result_type = reference_target(compact_result->type());
                    const auto *compact_value_type = type_cast<TimeSeriesValueType>(&compact_result_type);
                    if (compact_value_type != nullptr &&
                        &compact_value_type->value_type() == &item_value_type->value_type()) {
                        const auto concrete_compact_result = compact_wiring->complete_pending_wiring(*compact_result);
                        const auto *service_instances = compact_wiring->service_instances();
                        if (compact_wiring->inherited_requirements().empty() &&
                            compact_wiring->service_binding_requirements().empty() &&
                            (service_instances == nullptr || service_instances->instances().empty())) {
                            if (equivalent_wiring_bodies(*wiring, boundaries, concrete_result, *compact_wiring,
                                                         compact_inputs, concrete_compact_result,
                                                         reducer_input_types)) {
                                const auto compact_input_bindings = std::array{
                                    compact_inputs[0].record()->input_binding(compact_inputs[0].type()),
                                    compact_inputs[1].record()->input_binding(compact_inputs[1].type()),
                                };
                                if (!compact_input_bindings[0] || !compact_input_bindings[0]->is_borrowed() ||
                                    !compact_input_bindings[1] || !compact_input_bindings[1]->is_borrowed()) {
                                    throw WiringError{"a compact reduce input must use a borrowed input binding"};
                                }
                                const auto compact_input_indices = std::array{
                                    compact_input_bindings[0]->index(),
                                    compact_input_bindings[1]->index(),
                                };
                                auto output_input = std::optional<std::size_t>{};
                                auto compact_output_node = std::optional<std::size_t>{};
                                const auto *compact_output_record = concrete_compact_result.record();
                                const auto compact_output_binding =
                                    compact_output_record->input_binding(concrete_compact_result.type());
                                const TimeSeriesRuntimeType *compact_output_runtime_type{};
                                if (compact_output_binding && compact_output_binding->is_borrowed()) {
                                    output_input = compact_output_binding->index();
                                    compact_output_runtime_type =
                                        &resolve_time_series_runtime_type(concrete_compact_result.type());
                                } else {
                                    const auto &compact_output = compact_wiring->outputs()[
                                        compact_wiring->output_index(concrete_compact_result)];
                                    compact_output_runtime_type = compact_output.runtime_type;
                                    if (compact_output_runtime_type == nullptr) {
                                        compact_output_runtime_type =
                                            &resolve_time_series_runtime_type(*compact_output.type);
                                    }
                                    compact_output_node = compact_wiring->output_node_index(concrete_compact_result);
                                }
                                const auto compact_structure = graph_wiring_structure(
                                    *compact_wiring, std::span<const WiringPoint>{}, concrete_compact_result);
                                compact_wiring->finalize();
                                auto compact_node_indices = std::vector<std::size_t>{};
                                if (compact_output_node) {
                                    compact_node_indices.push_back(*compact_output_node);
                                }
                                auto compact_compiled_graph =
                                    compile_graph(*compact_wiring, allocator{}, compact_node_indices);
                                const auto borrowed_inputs = compact_compiled_graph->borrowed_input_bindings();
                                const auto &source_item_runtime_type = resolve_time_series_runtime_type(item_type);
                                const auto exact_borrowed_inputs =
                                    compact_compiled_graph->supports_borrowed_evaluation() &&
                                    borrowed_inputs.size() == compact_input_indices.size() &&
                                    compact_input_indices[0] != compact_input_indices[1] &&
                                    compact_input_indices[0] < borrowed_inputs.size() &&
                                    compact_input_indices[1] < borrowed_inputs.size() &&
                                    borrowed_inputs[compact_input_indices[0]].runtime_type ==
                                        &source_item_runtime_type &&
                                    borrowed_inputs[compact_input_indices[1]].runtime_type == &source_item_runtime_type;
                                auto compact_output_input = std::optional<std::size_t>{};
                                auto compatible_output = exact_borrowed_inputs;
                                if (compact_output_node) {
                                    compatible_output = compatible_output &&
                                        compact_output_runtime_type == &source_item_runtime_type &&
                                        compact_output_runtime_type ==
                                            borrowed_inputs[compact_input_indices[0]].runtime_type &&
                                        compact_output_runtime_type ==
                                            borrowed_inputs[compact_input_indices[1]].runtime_type;
                                } else if (output_input) {
                                    if (*output_input == compact_input_indices[0]) {
                                        compact_output_input = 0;
                                    } else if (*output_input == compact_input_indices[1]) {
                                        compact_output_input = 1;
                                    } else {
                                        compatible_output = false;
                                    }
                                    compatible_output = compatible_output &&
                                                       compact_output_runtime_type == &source_item_runtime_type;
                                } else {
                                    compatible_output = false;
                                }
                                if (compatible_output) {
                                    compact_graph.emplace(ReduceCompactGraphWiring{
                                        .graph = std::move(compact_compiled_graph),
                                        .input_indices = compact_input_indices,
                                        .output_node = compact_output_node
                                                           ? std::optional{compact_node_indices.front()}
                                                           : std::nullopt,
                                        .output_input = compact_output_input,
                                        .output_runtime_type = compact_output_runtime_type,
                                        .structure = compact_structure,
                                    });
                                }
                            }
                        }
                    }
                }
            } catch (const WiringError &) {
                // The full nested graph was already wired successfully; a rejected compact form falls back to it.
            }
        }
    }

    const auto output = wire_nested_output(concrete_result, concrete_result.type());
    wiring->finalize();
    auto input_nodes = std::array{wiring->output_node_index(boundaries[0]),
                                  wiring->output_node_index(boundaries[1])};
    const auto output_node = wiring->output_node_index(output);

    return {
        .structure = structure,
        .graph = {
            .nested = {
                .wiring = std::move(wiring),
                .input_nodes = input_nodes,
                .output_node = output_node,
            },
            .compact_graph = std::move(compact_graph),
        },
    };
}

} // namespace bg::details
