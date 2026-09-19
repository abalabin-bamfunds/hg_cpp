#pragma once

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <utility>
#include <vector>

namespace bg {

template <typename Graph, details::graph_code_argument Run, details::graph_code_argument... Arguments>
    requires std::derived_from<Graph, WiringTemplate> && requires {
        typename Graph::result_type;
    }
[[nodiscard]] auto nested(const Graph &graph, Run &&run, Arguments &&...arguments) {
    using Result = typename Graph::result_type;
    static_assert(details::is_typed_wiring_point<Result>, "a nested graph must have an output");
    using Target = typename details::typed_wiring_point<Result>::time_series_type;

    auto supplied =
        std::array<WiringArgument, sizeof...(Arguments)>{WiringArgument{std::forward<Arguments>(arguments)}...};
    auto &outer_wiring = WiringContext::current().wiring();
    auto normalized_result = details::probe_wiring_arguments(graph.signature(), supplied);
    auto &normalized = normalized_result.arguments;
    auto resolution = graph.signature().resolve_wiring_arguments(normalized, normalized_result.descriptors);
    const auto &resolved = resolution.signature;

    auto inner_wiring = std::make_shared<GraphWiring>();
    auto input_nodes = std::vector<std::size_t>{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner_wiring};
        auto inner_arguments = std::vector<WiringArgument>{};
        inner_arguments.reserve(normalized.size());
        const auto variadic = graph.signature().variadic_parameter();
        for (auto i = std::size_t{}; i < resolved.parameters.size(); ++i) {
            if (variadic && i == *variadic) {
                const auto &record = *details::variadic_wiring_record(normalized[i].time_series());
                if (record.direct_collection()) {
                    const auto &type = static_cast<const TimeSeriesType &>(*resolved.parameters[i].type);
                    const auto input = details::wire_nested_input(type);
                    input_nodes.push_back(details::wired_output_node_index(*inner_wiring, input));
                    auto argument = WiringArgument{input};
                    details::wiring_argument_access::set_variadic_name(argument, resolved.parameters[i].name);
                    inner_arguments.push_back(std::move(argument));
                    continue;
                }

                for (auto item = std::size_t{}; item < record.size(); ++item) {
                    auto argument = record.argument(item);
                    inner_arguments.push_back(details::wiring_argument_access::borrow(argument));
                    if (argument.is_time_series()) {
                        const auto &type = static_cast<const TimeSeriesType &>(argument.type());
                        const auto input = details::wire_nested_input(type);
                        input_nodes.push_back(details::wired_output_node_index(*inner_wiring, input));
                        details::wiring_argument_access::replace_time_series(inner_arguments.back(), input);
                    }
                }
                continue;
            }

            inner_arguments.push_back(details::wiring_argument_access::borrow(normalized[i]));
            if (resolved.parameters[i].type->is_time_series()) {
                const auto &type = static_cast<const TimeSeriesType &>(*resolved.parameters[i].type);
                const auto input = details::wire_nested_input(type);
                input_nodes.push_back(details::wired_output_node_index(*inner_wiring, input));
                details::wiring_argument_access::replace_time_series(inner_arguments.back(), input);
            }
        }

        const auto output = *static_cast<const WiringTemplate &>(graph)(
            resolution.type_map, std::span<const WiringArgument>{inner_arguments});
        const auto boundary_output =
            details::wire_nested_output(output, static_cast<const TimeSeriesType &>(*resolved.output));
        output_node = details::wired_output_node_index(*inner_wiring, boundary_output);
    }

    auto flattened = resolved;
    if (const auto variadic = graph.signature().variadic_parameter()) {
        const auto &record = *details::variadic_wiring_record(normalized[*variadic].time_series());
        if (record.direct_collection()) {
            flattened.parameters[*variadic].variadic_mode = details::VariadicMode::none;
        } else {
            flattened.parameters.erase(flattened.parameters.begin() + *variadic);
            auto insertion = *variadic;
            for (auto item = std::size_t{}; item < record.size(); ++item) {
                const auto argument = record.argument(item);
                if (argument.is_scalar()) {
                    continue;
                }
                flattened.parameters.insert(
                    flattened.parameters.begin() + insertion,
                    {*argument.name(), &argument.type(), active, optional, false,
                     details::VariadicMode::none});
                ++insertion;
            }
        }
    }

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
        if (graph.signature().variadic_parameter() && index == *graph.signature().variadic_parameter()) {
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
                outer_arguments.push_back(details::wiring_argument_access::borrow(argument));
                details::wiring_argument_access::clear_name(outer_arguments.back());
            }
            continue;
        }
        outer_arguments.push_back(details::wiring_argument_access::borrow(normalized[index]));
    }
    auto output_point = *owned_nested(std::span<const WiringArgument>{outer_arguments});
    return wp<REF<Target>>{output_point};
}

} // namespace bg
