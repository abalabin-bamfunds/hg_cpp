#pragma once

#include <bg/graph.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/reduce/details/default_reduce_wiring.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/util/named_argument.hpp>

#include <concepts>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Argument> struct reduce_callable_argument {
    using item_type = void;
};

template <typename Target, auto... Properties>
struct reduce_callable_argument<wp<REF<Target, Properties...>>> : reduce_callable_argument<wp<Target>> {};

template <typename Key, typename Item, auto... Properties>
struct reduce_callable_argument<wp<TSD<Key, Item, Properties...>>> {
    using item_type = wp<dereferenced_time_series_t<Item>>;
};

template <typename Argument> using reduce_callable_item_t = typename reduce_callable_argument<
    std::remove_cvref_t<Argument>>::item_type;

template <typename... Arguments> struct first_reduce_callable_item {
    using type = void;
};

template <typename First, typename... Rest> struct first_reduce_callable_item<First, Rest...> {
    using Candidate = reduce_callable_item_t<First>;
    using type = std::conditional_t<std::same_as<Candidate, void>,
                                    typename first_reduce_callable_item<Rest...>::type, Candidate>;
};

template <typename Function, typename Item> struct BoundReduceGraphFunction {
    Function function;

    decltype(auto) operator()(Item left, Item right) const {
        return std::invoke(function, std::move(left), std::move(right));
    }
};

template <typename Function, typename Item> auto bound_reduce_graph(Function function) {
    using Bound = BoundReduceGraphFunction<Function, Item>;
    auto signature = make_graph_signature<Bound>(util::symbol{"reduce_function"});
    return GraphTemplate{std::move(signature), Bound{std::move(function)}};
}

template <typename Argument> auto positional_reduce_argument(Argument &&argument) {
    if constexpr (is_named_argument_v<Argument>) {
        return std::tuple<>{};
    } else {
        return std::tuple<std::decay_t<Argument>>{std::forward<Argument>(argument)};
    }
}

template <typename Argument> auto named_reduce_argument(Argument &&argument) {
    if constexpr (is_named_argument_v<Argument>) {
        return std::tuple<std::decay_t<Argument>>{std::forward<Argument>(argument)};
    } else {
        return std::tuple<>{};
    }
}

template <typename... Arguments> auto group_reduce_arguments(Arguments &&...arguments) {
    auto positional = std::tuple_cat(positional_reduce_argument(std::forward<Arguments>(arguments))...);
    auto named = std::tuple_cat(named_reduce_argument(std::forward<Arguments>(arguments))...);
    return std::pair{std::move(positional), std::move(named)};
}

template <typename Tuple, std::size_t... Index>
void append_reduce_arguments(std::vector<WiringArgument> &result, Tuple &arguments,
                             std::index_sequence<Index...>) {
    (result.emplace_back(std::get<Index>(arguments)), ...);
}

template <typename SelectOverload, typename GraphArgumentValue, typename Positional, typename Named,
          std::size_t... P, std::size_t... N>
decltype(auto) select_reduce_overload(SelectOverload &&select_overload, const GraphArgumentValue &graph,
                                      Positional &positional, Named &named, std::index_sequence<P...>,
                                      std::index_sequence<N...>) {
    return std::invoke(std::forward<SelectOverload>(select_overload), graph, std::get<P>(positional)...,
                       std::get<N>(named)...);
}

template <typename SelectOverload, typename GraphArgumentValue, typename Positional, typename Named>
decltype(auto) select_reduce_overload(SelectOverload &&select_overload, const GraphArgumentValue &graph,
                                      Positional &positional, Named &named) {
    return select_reduce_overload(std::forward<SelectOverload>(select_overload), graph, positional, named,
                                  std::make_index_sequence<std::tuple_size_v<Positional>>{},
                                  std::make_index_sequence<std::tuple_size_v<Named>>{});
}

template <typename SelectOverload, typename Graph, typename... Arguments>
    requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate>
auto invoke_reduce(const TypeMap &type_map, SelectOverload &&select_overload, const Graph &function,
                   Arguments &&...arguments) {
    auto named_argument_seen = false;
    const auto validate_argument_order = [&](const auto &argument) {
        const auto named = [&] {
            if constexpr (is_named_argument_v<decltype(argument)>) {
                return true;
            } else if constexpr (std::same_as<std::remove_cvref_t<decltype(argument)>, WiringArgument>) {
                return argument.name().has_value();
            } else {
                return false;
            }
        }();
        if (named_argument_seen && !named) {
            throw WiringError{"a positional argument cannot follow a named argument"};
        }
        named_argument_seen |= named;
    };
    (validate_argument_order(arguments), ...);

    auto call = group_reduce_arguments(std::forward<Arguments>(arguments)...);
    auto supplied = std::vector<WiringArgument>{};
    supplied.reserve(std::tuple_size_v<decltype(call.first)> + std::tuple_size_v<decltype(call.second)>);
    append_reduce_arguments(supplied, call.first, std::make_index_sequence<std::tuple_size_v<decltype(call.first)>>{});
    append_reduce_arguments(supplied, call.second,
                           std::make_index_sequence<std::tuple_size_v<decltype(call.second)>>{});

    auto dictionary = std::optional<WiringArgument>{};
    auto default_value = std::optional<WiringArgument>{};
    auto positional_index = std::size_t{};
    for (const auto &argument : supplied) {
        const auto is_dictionary = argument.name() == std::optional{util::symbol{"values"}} ||
                                   (!argument.name() && positional_index == 0);
        const auto is_default = argument.name() == std::optional{util::symbol{"default"}} ||
                                (!argument.name() && positional_index == 1);
        if (is_dictionary) {
            if (dictionary) {
                throw WiringError{"reduce accepts one TSD argument"};
            }
            dictionary = argument;
        } else if (is_default) {
            if (default_value) {
                throw WiringError{"reduce accepts one default value"};
            }
            default_value = argument;
        } else {
            throw WiringError{"reduce accepts a TSD argument and an optional default"};
        }
        if (!argument.name()) {
            ++positional_index;
        }
    }
    if (!dictionary) {
        throw WiringError{"reduce requires a TSD argument"};
    }

    auto graph_argument = build_reduce_graph_argument(function, *dictionary, default_value, type_map);
    return select_reduce_overload(std::forward<SelectOverload>(select_overload), graph_argument, call.first,
                                  call.second);
}

struct reduce_operator_implementation {
    template <typename SelectOverload, typename Graph, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate>
    static auto invoke(const TypeMap &type_map, SelectOverload &&select_overload, const Graph &function,
                       Arguments &&...arguments) {
        return invoke_reduce(type_map, std::forward<SelectOverload>(select_overload), function,
                             std::forward<Arguments>(arguments)...);
    }

    template <typename SelectOverload, typename Function, typename... Arguments>
        requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
    static auto invoke(const TypeMap &type_map, SelectOverload &&select_overload, Function &&function,
                       Arguments &&...arguments) {
        using Item = typename first_reduce_callable_item<Arguments...>::type;
        if constexpr (std::same_as<Item, void>) {
            throw WiringError{"a reduce lambda requires a typed TSD argument; use a graph template otherwise"};
        } else {
            using StoredFunction = std::decay_t<Function>;
            auto graph = bound_reduce_graph<StoredFunction, Item>(StoredFunction{std::forward<Function>(function)});
            return invoke_reduce(type_map, std::forward<SelectOverload>(select_overload), graph,
                                 std::forward<Arguments>(arguments)...);
        }
    }
};

} // namespace bg::details
