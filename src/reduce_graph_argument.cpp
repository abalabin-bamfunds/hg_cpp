#include <bg/reduce/details/reduce_graph_argument.hpp>

#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
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

} // namespace

ReduceGraphArgumentWiring build_reduce_graph_argument_wiring(const WiringTemplate &function,
                                                              const TimeSeriesType &item_type,
                                                              const TypeMap &type_map) {
    const auto *item_value_type = type_cast<TimeSeriesValueType>(&item_type);
    if (item_value_type == nullptr) {
        throw WiringError{"reduce requires TSD items to be scalar-valued time series"};
    }
    validate_signature(function);

    auto wiring = std::make_shared<GraphWiring>(WiringContext::current().wiring());
    auto context = WiringContext{*wiring};

    auto boundaries = std::array{wire_nested_input(item_type), wire_nested_input(item_type)};
    auto arguments = std::array{WiringArgument{boundaries[0]}, WiringArgument{boundaries[1]}};
    const auto result = function(type_map, std::span<const WiringArgument>{arguments});
    if (!result) {
        throw WiringError{"a reduce graph must produce an output"};
    }
    const auto &result_type = reference_target(result->type());
    const auto *result_value_type = type_cast<TimeSeriesValueType>(&result_type);
    if (result_value_type == nullptr || item_value_type == nullptr ||
        &result_value_type->value_type() != &item_value_type->value_type() || !item_type.accepts(result_type)) {
        throw WiringError{"a reduce graph output must have the TSD item value type"};
    }

    const auto structure = graph_wiring_structure(*wiring, boundaries, result);
    const auto &output_type = reference_target(result->type());
    const auto output = wire_nested_output(*result, result->type());

    auto input_nodes = std::array{wiring->output_node_index(boundaries[0]),
                                  wiring->output_node_index(boundaries[1])};
    const auto output_node = wiring->output_node_index(output);

    return {
        .structure = structure,
        .graph = {
            .wiring = std::move(wiring),
            .input_nodes = input_nodes,
            .output_node = output_node,
            .output_type = &output_type,
        },
    };
}

} // namespace bg::details
