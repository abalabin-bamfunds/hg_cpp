#pragma once

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <utility>
#include <vector>

namespace bg {

template <typename Graph, details::graph_code_argument Run, details::graph_code_argument... Arguments>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
[[nodiscard]] auto nested(const Graph &graph, Run &&run, Arguments &&...arguments) {
    using Result = typename Graph::result_type;
    static_assert(details::is_typed_wiring_point<Result>, "a nested graph must have an output");
    using Target = typename details::typed_wiring_point<Result>::time_series_type;

    auto supplied =
        std::array<WiringArgument, sizeof...(Arguments)>{WiringArgument{std::forward<Arguments>(arguments)}...};
    auto &outer_wiring = WiringContext::current().wiring();
    auto temporary_records = details::WiringRecordOverlay{outer_wiring};
    auto normalized_result = details::probe_wiring_arguments(graph.signature(), supplied, temporary_records);
    auto &normalized = normalized_result.arguments;
    auto resolution = graph.signature().resolve_wiring_arguments(normalized, normalized_result.descriptors);
    const auto &resolved = resolution.signature;

    const auto outer_diagnostics = details::WiringDiagnosticContext::current(outer_wiring);
    auto inner_wiring = std::make_shared<GraphWiring>(outer_wiring);
    auto diagnostics = details::WiringDiagnosticContext{*inner_wiring, outer_diagnostics};
    auto input_boundaries = std::vector<WiringPoint>{};
    auto flattened_parameters = std::vector<ResolvedParameter>{};
    flattened_parameters.reserve(resolved.parameters.size());
    auto output_node = std::size_t{};
    auto boundary_output = WiringPoint{};
    {
        auto context = WiringContext{*inner_wiring};
        // Make resolved graph context parameters available as inner contexts while wiring the graph body.
        auto context_scopes = details::ContextScopes::activate_context_parameters(resolved, normalized);
        auto inner_arguments = std::vector<WiringArgument>{};
        inner_arguments.reserve(normalized.size());
        for (auto i = std::size_t{}; i < resolved.parameters.size(); ++i) {
            if (resolved.parameters[i].context) {
                continue;
            }
            if (resolved.parameters[i].variadic_mode != details::VariadicMode::none) {
                const auto &record = *details::variadic_wiring_record(normalized[i].time_series());
                if (record.direct_collection()) {
                    const auto direct = *record.direct_collection();
                    const auto &type = static_cast<const TimeSeriesType &>(direct.type());
                    const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
                    const auto &input_reference = reference != nullptr
                                                      ? *reference
                                                      : time_series_reference_type(type, TimeSeriesGuarantee::const_);
                    const auto input = details::wire_nested_input(input_reference);
                    input_boundaries.push_back(input);
                    auto parameter = resolved.parameters[i];
                    parameter.type = &input.type();
                    parameter.variadic_mode = details::VariadicMode::none;
                    flattened_parameters.push_back(parameter);
                    auto argument = WiringArgument{input};
                    argument.set_variadic_name(resolved.parameters[i].name, resolved.parameters[i].variadic_mode);
                    inner_arguments.push_back(std::move(argument));
                    continue;
                }

                for (auto item = std::size_t{}; item < record.size(); ++item) {
                    auto argument = record.argument(item);
                    inner_arguments.push_back(WiringArgument::borrow(argument));
                    if (argument.is_time_series()) {
                        const auto &type = static_cast<const TimeSeriesType &>(argument.type());
                        const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
                        const auto &input_reference = reference != nullptr
                                                          ? *reference
                                                          : time_series_reference_type(type, TimeSeriesGuarantee::const_);
                        const auto input = details::wire_nested_input(input_reference);
                        input_boundaries.push_back(input);
                        inner_arguments.back().replace_time_series(input);
                        flattened_parameters.push_back(
                            {*argument.name(), &input.type(), active, optional, false, details::VariadicMode::none});
                    }
                }
                continue;
            }

            inner_arguments.push_back(WiringArgument::borrow(normalized[i]));
            if (resolved.parameters[i].type->is_time_series()) {
                const auto &type = static_cast<const TimeSeriesType &>(normalized[i].type());
                const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
                const auto &input_reference = reference != nullptr
                                                  ? *reference
                                                  : time_series_reference_type(type, TimeSeriesGuarantee::const_);
                const auto input = details::wire_nested_input(input_reference);
                input_boundaries.push_back(input);
                inner_arguments.back().replace_time_series(input);
                auto parameter = resolved.parameters[i];
                parameter.type = &input.type();
                flattened_parameters.push_back(parameter);
            } else {
                flattened_parameters.push_back(resolved.parameters[i]);
            }
        }

        const auto result = *static_cast<const WiringTemplate &>(graph)(
            resolution.type_map, std::span<const WiringArgument>{inner_arguments});
        boundary_output = details::wire_nested_output(result, static_cast<const TimeSeriesType &>(*resolved.output));
    }

    inner_wiring->finalize();
    auto input_nodes = std::vector<std::size_t>{};
    input_nodes.reserve(input_boundaries.size());
    for (const auto input : input_boundaries) {
        input_nodes.push_back(inner_wiring->output_node_index(input));
    }
    output_node = inner_wiring->output_node_index(boundary_output);

    auto flattened = ResolvedSignature{resolved.name, std::move(flattened_parameters), resolved.output};

    for (const auto &requirement : inner_wiring->inherited_requirements()) {
        flattened.parameters.push_back({requirement.name, &requirement.boundary_input_type(), passive, optional});
    }
    auto inherited_providers = std::vector<WiringPoint>{};
    inherited_providers.reserve(inner_wiring->inherited_requirements().size());
    for (const auto &requirement : inner_wiring->inherited_requirements()) {
        inherited_providers.push_back(requirement.provider);
    }
    const auto *inner_graph_wiring = inner_wiring.get();

    auto nested_wiring = details::NestedGraphWiring{
        .wiring = std::move(inner_wiring),
        .input_nodes = std::move(input_nodes),
        .output_node = output_node,
    };
    auto node_template = details::make_nested_node(std::move(nested_wiring), flattened);
    const auto &owned_nested = outer_wiring.own_node_template(std::move(node_template));
    auto outer_arguments = std::vector<WiringArgument>{};
    outer_arguments.reserve(flattened.parameters.size() + 1);
    outer_arguments.emplace_back(std::forward<Run>(run));
    for (auto index = std::size_t{}; index < normalized.size(); ++index) {
        if (resolved.parameters[index].context) {
            continue;
        }
        if (resolved.parameters[index].variadic_mode != details::VariadicMode::none) {
            const auto &record = *details::variadic_wiring_record(normalized[index].time_series());
            if (const auto collection = record.direct_collection()) {
                outer_arguments.emplace_back(*collection);
                continue;
            }

            for (auto item = std::size_t{}; item < record.size(); ++item) {
                const auto argument = record.argument(item);
                if (argument.is_scalar()) {
                    continue;
                }
                outer_arguments.push_back(WiringArgument::borrow(argument));
                outer_arguments.back().clear_name();
            }
            continue;
        }
        outer_arguments.push_back(WiringArgument::borrow(normalized[index]));
    }
    for (const auto provider : inherited_providers) {
        outer_arguments.emplace_back(provider);
    }
    auto call = outer_wiring.invoke_node(owned_nested, outer_arguments);
    if (!call.output) {
        throw WiringError{"a nested graph node did not produce its declared output"};
    }
    outer_wiring.register_nested_service_consumers(*inner_graph_wiring, call.identity);
    return wp<REF<Target>>{*call.output};
}

} // namespace bg
