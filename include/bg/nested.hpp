#pragma once

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <std::size_t Index, typename Argument>
decltype(auto) nested_graph_argument(Argument &argument, const std::vector<std::optional<WiringPoint>> &inputs) {
    if constexpr (std::same_as<std::remove_cvref_t<Argument>, WiringPoint> || is_typed_wiring_point<Argument>) {
        return *inputs[Index];
    } else if constexpr (free_time_series_list<Argument>) {
        return *inputs[Index];
    } else if constexpr (free_time_series_bundle<Argument>) {
        return *inputs[Index];
    } else {
        return (argument);
    }
}

template <typename Graph, typename Arguments, std::size_t... Index>
[[nodiscard]] WiringPoint invoke_nested_graph(const Graph &graph, Arguments &arguments,
                                              const std::vector<std::optional<WiringPoint>> &inputs,
                                              std::index_sequence<Index...>) {
    return static_cast<WiringPoint>(graph(nested_graph_argument<Index>(std::get<Index>(arguments), inputs)...));
}

} // namespace bg::details

namespace bg {

template <typename Graph, details::graph_code_argument Run, details::graph_code_argument... Arguments>
    requires std::derived_from<Graph, WiringTemplate> && requires { typename Graph::result_type; }
[[nodiscard]] auto nested(const Graph &graph, Run &&run, Arguments &&...arguments) {
    using Result = typename Graph::result_type;
    static_assert(details::is_typed_wiring_point<Result>, "a nested graph must have an output");
    using Target = typename details::typed_wiring_point<Result>::time_series_type;

    auto supplied = std::array<details::wiring_argument, sizeof...(Arguments)>{details::wiring_argument{arguments}...};
    auto argument_types = std::vector<const Type *>{};
    argument_types.reserve(supplied.size());
    for (const auto &argument : supplied) {
        if (argument.is_wiring_point()) {
            argument_types.push_back(&argument.wiring_point().type());
        } else if (argument.is_free_time_series_list()) {
            argument_types.push_back(argument.free_time_series_list().type);
        } else if (argument.is_free_time_series_bundle()) {
            argument_types.push_back(argument.free_time_series_bundle().type);
        } else {
            argument_types.push_back(&argument.scalar_type());
        }
    }
    const auto resolved = graph.signature().resolve(argument_types);

    auto inner_wiring = std::make_shared<GraphWiring>();
    auto inputs = std::vector<std::optional<WiringPoint>>(sizeof...(Arguments));
    auto input_nodes = std::vector<std::size_t>{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner_wiring};
        for (auto i = std::size_t{}; i < resolved.parameters.size(); ++i) {
            if (!resolved.parameters[i].type->is_time_series()) {
                continue;
            }

            inputs[i] = details::wire_nested_input(static_cast<const TimeSeriesType &>(*resolved.parameters[i].type));
            input_nodes.push_back(inputs[i]->node_index());
        }

        auto originals = std::forward_as_tuple(arguments...);
        const auto output =
            details::invoke_nested_graph(graph, originals, inputs, std::index_sequence_for<Arguments...>{});
        const auto boundary_output = details::wire_nested_output(output);
        output_node = boundary_output.node_index();
    }

    auto nested_wiring = details::NestedGraphWiring{
        .wiring = std::move(inner_wiring),
        .input_nodes = std::move(input_nodes),
        .output_node = output_node,
    };
    auto node_template = details::make_nested_node(std::move(nested_wiring), resolved);
    auto &outer_wiring = WiringContext::current().wiring();
    const auto &owned_nested = outer_wiring.own_node_template(std::move(node_template));
    auto output_point = *owned_nested(std::forward<Run>(run), std::forward<Arguments>(arguments)...);
    return wp<REF<Target>>{output_point};
}

} // namespace bg
