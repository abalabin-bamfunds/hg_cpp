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
    (std::size_t{} + ... + static_cast<std::size_t>(!is_named_argument_v<Arguments> &&
                                                     !is_callable_requirement_v<Arguments>));

template <typename First, typename... Rest> decltype(auto) unnamed_argument(First &&first, Rest &&...rest) {
    if constexpr (!is_named_argument_v<First> && !is_callable_requirement_v<First>) {
        return std::forward<First>(first);
    } else {
        return unnamed_argument(std::forward<Rest>(rest)...);
    }
}

template <typename T>
inline constexpr bool supported_callable_graph_argument =
    !is_named_argument_v<T> || named_argument_is<"name", T> || named_argument_is<"label", T>;

template <typename... Arguments>
inline constexpr std::size_t callable_graph_requirement_count =
    (std::size_t{} + ... + static_cast<std::size_t>(is_callable_requirement_v<Arguments>));

template <typename First, typename... Rest> decltype(auto) callable_graph_requirement(First &&first, Rest &&...rest) {
    if constexpr (is_callable_requirement_v<First>) {
        return std::forward<First>(first);
    } else {
        return callable_graph_requirement(std::forward<Rest>(rest)...);
    }
}

template <typename T> inline constexpr bool supported_aggregate_graph_argument = named_argument_is<"label", T>;

template <typename... TypeParameters, typename Function, typename... Arguments>
[[nodiscard]] auto make_callable_graph(Function &&function, Arguments &&...arguments) {
    static_assert((supported_callable_graph_argument<Arguments> && ...), "unsupported named graph argument");
    static_assert(named_argument_count<"name", Arguments...> <= 1, "a lambda graph accepts one name argument");
    static_assert(named_argument_count<"label", Arguments...> <= 1, "a graph accepts one label metadata argument");
    static_assert(callable_graph_requirement_count<Arguments...> <= 1,
                  "a graph accepts one requirement argument");

    using StoredFunction = std::decay_t<Function>;
    const auto name = [&] {
        if constexpr (named_argument_count<"name", Arguments...> == 1) {
            return wiring_label(named_argument_value<"name">(std::forward<Arguments>(arguments)...));
        } else {
            return util::symbol{util::unqualified_type_name<StoredFunction>()};
        }
    }();
    auto signature = make_graph_signature<StoredFunction>(name);

    if constexpr (callable_graph_requirement_count<Arguments...> == 0 &&
                  named_argument_count<"label", Arguments...> == 0) {
        return GraphTemplate<StoredFunction, default_wiring_label, TypeParameters...>{
            std::move(signature), std::forward<Function>(function)};
    } else if constexpr (callable_graph_requirement_count<Arguments...> == 0) {
        using Label = std::decay_t<decltype(named_argument_value<"label">(
            std::forward<Arguments>(arguments)...))>;
        return GraphTemplate<StoredFunction, Label, TypeParameters...>{
            std::move(signature), std::forward<Function>(function),
            named_argument_value<"label">(std::forward<Arguments>(arguments)...)};
    } else if constexpr (named_argument_count<"label", Arguments...> == 0) {
        auto requirement = callable_graph_requirement(std::forward<Arguments>(arguments)...);
        using Requirement = std::decay_t<decltype(requirement.function)>;
        using Options = CallableGraphOptions<default_wiring_label, Requirement>;
        return GraphTemplate<StoredFunction, Options, TypeParameters...>{
            std::move(signature), std::forward<Function>(function),
            Options{default_wiring_label{}, std::move(requirement)}};
    } else {
        auto requirement = callable_graph_requirement(std::forward<Arguments>(arguments)...);
        using Requirement = std::decay_t<decltype(requirement.function)>;
        using Label = std::decay_t<decltype(named_argument_value<"label">(
            std::forward<Arguments>(arguments)...))>;
        using Options = CallableGraphOptions<Label, Requirement>;
        return GraphTemplate<StoredFunction, Options, TypeParameters...>{
            std::move(signature), std::forward<Function>(function),
            Options{named_argument_value<"label">(std::forward<Arguments>(arguments)...), std::move(requirement)}};
    }
}

template <typename Definition, typename... TypeParameters, typename... Arguments>
[[nodiscard]] auto make_aggregate_graph(Arguments &&...arguments) {
    static_assert((supported_aggregate_graph_argument<Arguments> && ...), "unsupported named graph argument");
    static_assert(named_argument_count<"label", Arguments...> <= 1, "a graph accepts one label metadata argument");

    using Function = aggregate_graph_function_t<Definition>;
    auto signature = aggregate_graph_signature<Definition>();

    if constexpr (named_argument_count<"label", Arguments...> == 0) {
        return GraphTemplate<Function, default_wiring_label, TypeParameters...>{std::move(signature), Function{}};
    } else {
        using Label = std::decay_t<decltype(named_argument_value<"label">(
            std::forward<Arguments>(arguments)...))>;
        return GraphTemplate<Function, Label, TypeParameters...>{
            std::move(signature), Function{},
            named_argument_value<"label">(std::forward<Arguments>(arguments)...)};
    }
}

} // namespace bg::details
