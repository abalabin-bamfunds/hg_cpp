#pragma once

#include <hg/graph/compiled_graph.hpp>
#include <hg/graph/details/nested.hpp>
#include <hg/graph/details/normalized_wiring_arguments.hpp>
#include <hg/graph/details/variadic_wiring_record.hpp>
#include <hg/graph/details/wiring_record_overlay.hpp>
#include <hg/graph/graph_template.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/graph/wiring_error.hpp>
#include <hg/node/node_template.hpp>
#include <hg/time_series/time_series_type.hpp>

#include <concepts>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace hg::nested_details {

template <typename Graph, details::graph_code_argument Run>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
auto wire(const Graph &graph, const TypeMap *seed_type_map, Run &&run,
          std::span<const WiringArgument> supplied) {
    using Result = typename Graph::result_type;
    static_assert(details::is_typed_wiring_point<Result>, "a nested graph must have an output");
    using Target = details::dereferenced_time_series_t<typename details::typed_wiring_point<Result>::time_series_type>;

    auto &outer_wiring = WiringContext::current().wiring();
    auto temporary_records = details::WiringRecordOverlay{outer_wiring};
    auto normalized_result = details::normalize_wiring_arguments(
        graph.signature(), std::span<const WiringArgument>{supplied}, outer_wiring.allocation(), temporary_records);
    auto &normalized = normalized_result.arguments;
    const auto &signature = graph.signature();
    auto parameters = std::vector<ResolvedParameter>{};
    parameters.reserve(signature.parameter_count());
    for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
        const auto &parameter = signature.parameter(index);
        parameters.push_back({.name = parameter.name,
                              .type = &normalized[index].type(),
                              .activity = parameter.activity,
                              .validity = parameter.validity,
                              .accepts_absent_scalar = parameter.accepts_absent_scalar,
                              .variadic_mode = parameter.variadic_mode,
                              .context = parameter.context,
                              .dynamic_activity = parameter.dynamic_activity,
                              .valid_items = parameter.valid_items});
    }
    auto input_signature = ResolvedSignature{signature.name(), std::move(parameters), nullptr};

    const auto outer_diagnostics = details::WiringDiagnosticContext::current(outer_wiring);
    auto inner_wiring = std::make_shared<GraphWiring>(outer_wiring);
    auto diagnostics = details::WiringDiagnosticContext{*inner_wiring, outer_diagnostics};
    auto input_boundaries = std::vector<WiringPoint>{};
    auto flattened_parameters = std::vector<ResolvedParameter>{};
    flattened_parameters.reserve(input_signature.parameters.size());
    auto output_node = std::size_t{};
    auto boundary_output = WiringPoint{};
    {
        auto context = WiringContext{*inner_wiring};
        // Make graph context parameters available while the inner call resolves its omitted arguments.
        auto context_scopes = details::ContextScopes::activate_context_parameters(input_signature, normalized);
        auto inner_arguments = std::vector<WiringArgument>{};
        inner_arguments.reserve(normalized.size());
        for (auto i = std::size_t{}; i < input_signature.parameters.size(); ++i) {
            if (input_signature.parameters[i].context) {
                continue;
            }
            if (input_signature.parameters[i].variadic_mode != details::VariadicMode::none) {
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
                    auto parameter = input_signature.parameters[i];
                    parameter.type = &input.type();
                    parameter.variadic_mode = details::VariadicMode::none;
                    flattened_parameters.push_back(parameter);
                    auto argument = WiringArgument{input};
                    argument.set_variadic_name(input_signature.parameters[i].name,
                                               input_signature.parameters[i].variadic_mode);
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
                        flattened_parameters.push_back({.name = *argument.name(),
                                                        .type = &input.type(),
                                                        .activity = active,
                                                        .validity = optional,
                                                        .valid_items = argument.annotations().valid_items});
                    }
                }
                continue;
            }

            inner_arguments.push_back(WiringArgument::borrow(normalized[i]));
            if (normalized[i].type().is_time_series()) {
                const auto &type = static_cast<const TimeSeriesType &>(normalized[i].type());
                const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
                const auto &input_reference = reference != nullptr
                                                  ? *reference
                                                  : time_series_reference_type(type, TimeSeriesGuarantee::const_);
                const auto input = details::wire_nested_input(input_reference);
                input_boundaries.push_back(input);
                inner_arguments.back().replace_time_series(input);
                auto parameter = input_signature.parameters[i];
                parameter.type = &input.type();
                flattened_parameters.push_back(parameter);
            } else {
                flattened_parameters.push_back(input_signature.parameters[i]);
            }
        }

        const auto result = *inner_wiring->invoke(static_cast<const WiringTemplate &>(graph),
                                                  std::span<const WiringArgument>{inner_arguments}, seed_type_map);
        boundary_output = details::wire_nested_output(result, result.type());
        input_signature.output = &result.type();
    }

    inner_wiring->finalize();
    auto input_nodes = std::vector<std::size_t>{};
    input_nodes.reserve(input_boundaries.size());
    for (const auto input : input_boundaries) {
        input_nodes.push_back(inner_wiring->output_node_index(input));
    }
    output_node = inner_wiring->output_node_index(boundary_output);

    auto flattened = ResolvedSignature{input_signature.name, std::move(flattened_parameters), input_signature.output};

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
        if (input_signature.parameters[index].context) {
            continue;
        }
        if (input_signature.parameters[index].variadic_mode != details::VariadicMode::none) {
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

} // namespace hg::nested_details

namespace hg {

template <typename Graph, details::graph_code_argument Run, details::graph_code_argument... Arguments>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
[[nodiscard]] auto nested(const Graph &graph, Run &&run, Arguments &&...arguments) {
    auto supplied = details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
    return nested_details::wire(graph, nullptr, std::forward<Run>(run),
                                std::span<const WiringArgument>{supplied});
}

template <typename Graph, details::graph_code_argument Run>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
[[nodiscard]] auto nested(const Graph &graph, Run &&run, std::span<const WiringArgument> arguments) {
    return nested_details::wire(graph, nullptr, std::forward<Run>(run), arguments);
}

template <typename Graph, typename... TypeParameters, details::graph_code_argument Run,
          details::graph_code_argument... Arguments>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
[[nodiscard]] auto nested(const details::MappedWiring<Graph, TypeParameters...> &graph, Run &&run,
                          Arguments &&...arguments) {
    auto supplied = details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
    return nested_details::wire(graph.target(), &graph.type_map(), std::forward<Run>(run),
                                std::span<const WiringArgument>{supplied});
}

template <typename Graph, typename... TypeParameters, details::graph_code_argument Run>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
[[nodiscard]] auto nested(const details::MappedWiring<Graph, TypeParameters...> &graph, Run &&run,
                          std::span<const WiringArgument> arguments) {
    return nested_details::wire(graph.target(), &graph.type_map(), std::forward<Run>(run), arguments);
}

} // namespace hg
