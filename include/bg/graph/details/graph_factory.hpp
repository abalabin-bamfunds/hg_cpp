#pragma once

#include <bg/graph/details/aggregate_graph.hpp>
#include <bg/graph/graph_template.hpp>
#include <bg/util/named_argument.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename... Arguments>
inline constexpr std::size_t unnamed_argument_count =
    (std::size_t{} + ... + static_cast<std::size_t>(!is_named_argument_v<Arguments>));

template <typename First, typename... Rest> decltype(auto) unnamed_argument(First &&first, Rest &&...rest) {
    if constexpr (!is_named_argument_v<First>) {
        return std::forward<First>(first);
    } else {
        return unnamed_argument(std::forward<Rest>(rest)...);
    }
}

template <typename T>
inline constexpr bool supported_callable_graph_argument =
    !is_named_argument_v<T> || named_argument_is<"name", T> || named_argument_is<"label", T>;

template <typename T> inline constexpr bool supported_aggregate_graph_argument = named_argument_is<"label", T>;

template <typename Function, typename... Arguments>
[[nodiscard]] auto make_callable_graph(Function &&function, Arguments &&...arguments) {
    static_assert((supported_callable_graph_argument<Arguments> && ...), "unsupported named graph argument");
    static_assert(named_argument_count<"name", Arguments...> == 1, "a lambda graph requires one name argument");
    static_assert(named_argument_count<"label", Arguments...> <= 1, "a graph accepts one label metadata argument");

    using StoredFunction = std::decay_t<Function>;
    auto signature = make_graph_signature<StoredFunction>(
        wiring_label(named_argument_value<"name">(std::forward<Arguments>(arguments)...)));

    if constexpr (named_argument_count<"label", Arguments...> == 0) {
        return GraphTemplate{std::move(signature), std::forward<Function>(function)};
    } else {
        return GraphTemplate{std::move(signature), std::forward<Function>(function),
                             named_argument_value<"label">(std::forward<Arguments>(arguments)...)};
    }
}

template <typename Definition, typename... Arguments>
[[nodiscard]] auto make_aggregate_graph(Arguments &&...arguments) {
    static_assert((supported_aggregate_graph_argument<Arguments> && ...), "unsupported named graph argument");
    static_assert(named_argument_count<"label", Arguments...> <= 1, "a graph accepts one label metadata argument");

    using Function = aggregate_graph_function_t<Definition>;
    auto signature = aggregate_graph_signature<Definition>();

    if constexpr (named_argument_count<"label", Arguments...> == 0) {
        return GraphTemplate{std::move(signature), Function{}};
    } else {
        return GraphTemplate{std::move(signature), Function{},
                             named_argument_value<"label">(std::forward<Arguments>(arguments)...)};
    }
}

} // namespace bg::details
