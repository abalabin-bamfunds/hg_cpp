#pragma once

#include <bg/graph.hpp>
#include <bg/map/details/map_call.hpp>
#include <bg/map/details/map_plan.hpp>
#include <bg/operator.hpp>

#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename T> struct is_pass_through_argument : std::false_type {};

template <typename T> struct is_pass_through_argument<pass_through_argument<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_pass_through_argument_v = is_pass_through_argument<std::remove_cvref_t<T>>::value;

template <typename Argument> decltype(auto) unwrap_map_argument(Argument &&argument) {
    if constexpr (is_pass_through_argument_v<Argument>) {
        return std::forward<Argument>(argument).value;
    } else {
        return std::forward<Argument>(argument);
    }
}

template <util::compile_time_string Name, typename Value>
auto unwrap_map_argument(named_argument<Name, Value> &argument) {
    if constexpr (is_pass_through_argument_v<Value>) {
        return named_argument<Name, std::remove_cvref_t<decltype(argument.value.value)>>{argument.value.value};
    } else {
        return (argument);
    }
}

template <typename Argument> MapSuppliedArgument map_supplied_argument(Argument &argument) {
    if constexpr (is_named_argument_v<Argument>) {
        auto unwrapped = unwrap_map_argument(argument);
        return {WiringArgument{unwrapped}, is_pass_through_argument_v<decltype(argument.value)>};
    } else {
        return {WiringArgument{unwrap_map_argument(argument)}, is_pass_through_argument_v<Argument>};
    }
}

template <typename Tuple, std::size_t... Index>
std::vector<MapSuppliedArgument> map_supplied_arguments(Tuple &tuple, std::index_sequence<Index...>) {
    auto result = std::vector<MapSuppliedArgument>{};
    result.reserve(sizeof...(Index));
    (result.push_back(map_supplied_argument(std::get<Index>(tuple))), ...);
    return result;
}

template <typename Tuple> std::vector<MapSuppliedArgument> map_supplied_arguments(Tuple &tuple) {
    return map_supplied_arguments(tuple, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename SelectOverload, typename Positional, typename Named, typename Options,
          std::size_t... P, std::size_t... N, std::size_t... O>
decltype(auto) select_map_overload(SelectOverload &&select_overload, const GraphArgument &function,
                                   Positional &positional, Named &named, Options &options,
                                   std::index_sequence<P...>, std::index_sequence<N...>,
                                   std::index_sequence<O...>) {
    return std::invoke(std::forward<SelectOverload>(select_overload), function,
                       unwrap_map_argument(std::get<P>(positional))...,
                       unwrap_map_argument(std::get<N>(named))..., std::get<O>(options)...);
}

template <typename SelectOverload, typename Positional, typename Named, typename Options>
decltype(auto) select_map_overload(SelectOverload &&select_overload, const GraphArgument &function,
                                   Positional &positional, Named &named, Options &options) {
    return select_map_overload(std::forward<SelectOverload>(select_overload), function, positional,
                               named, options,
                               std::make_index_sequence<std::tuple_size_v<Positional>>{},
                               std::make_index_sequence<std::tuple_size_v<Named>>{},
                               std::make_index_sequence<std::tuple_size_v<Options>>{});
}

template <typename T> struct map_graph_function_parameter {
    using type = typename map_argument_traits_t<T>::per_key_type;
};

template <util::compile_time_string Name, typename T>
struct map_graph_function_parameter<named_argument<Name, T>> {
    using type = typename map_argument_traits_t<T>::per_key_type;
};

template <typename T>
using map_graph_function_parameter_t =
    typename map_graph_function_parameter<std::remove_cvref_t<T>>::type;

template <typename Function, typename... Arguments> struct BoundMapGraphFunction {
    Function function;

    decltype(auto) operator()(Arguments... arguments) const {
        return std::invoke(function, std::move(arguments)...);
    }
};

enum class MapGraphFunctionResult : unsigned char {
    invalid,
    sink,
    value,
    inconsistent,
};

consteval MapGraphFunctionResult combine_map_graph_function_results(MapGraphFunctionResult left,
                                                                    MapGraphFunctionResult right) {
    if (left == MapGraphFunctionResult::invalid) {
        return right;
    }
    if (right == MapGraphFunctionResult::invalid) {
        return left;
    }
    return left == right ? left : MapGraphFunctionResult::inconsistent;
}

template <typename Function, typename... Arguments> consteval MapGraphFunctionResult map_graph_function_result() {
    if constexpr (!std::invocable<const Function &, Arguments...>) {
        return MapGraphFunctionResult::invalid;
    } else {
        using Result = std::invoke_result_t<const Function &, Arguments...>;
        if constexpr (std::same_as<Result, void>) {
            return MapGraphFunctionResult::sink;
        } else if constexpr (is_typed_wiring_point<Result>) {
            return MapGraphFunctionResult::value;
        } else {
            return MapGraphFunctionResult::inconsistent;
        }
    }
}

template <std::size_t Index, bool HasKey, bool KeyInserted, bool SetOmitted, bool OmitSoleSet,
          typename Supplied, typename Function, typename Key, typename... Parameters>
consteval MapGraphFunctionResult map_graph_function_result() {
    auto result = MapGraphFunctionResult::invalid;
    if constexpr (HasKey && !KeyInserted) {
        result = combine_map_graph_function_results(
            result,
            map_graph_function_result<Index, HasKey, true, SetOmitted, OmitSoleSet, Supplied, Function, Key,
                                      Parameters..., Key>());
    }

    if constexpr (Index == std::tuple_size_v<Supplied>) {
        if constexpr (!HasKey || KeyInserted) {
            result = combine_map_graph_function_results(
                result, map_graph_function_result<Function, Parameters...>());
        }
        return result;
    } else {
        using Argument = std::tuple_element_t<Index, Supplied>;
        if constexpr (!SetOmitted && map_key_set_candidate<Argument>) {
            result = combine_map_graph_function_results(
                result,
                map_graph_function_result<Index + 1, HasKey, KeyInserted, true, OmitSoleSet, Supplied,
                                          Function, Key, Parameters...>());
        }
        if constexpr (!OmitSoleSet || SetOmitted || !map_key_set_candidate<Argument>) {
            using Parameter = map_graph_function_parameter_t<Argument>;
            result = combine_map_graph_function_results(
                result,
                map_graph_function_result<Index + 1, HasKey, KeyInserted, SetOmitted, OmitSoleSet,
                                          Supplied, Function, Key, Parameters..., Parameter>());
        }
        return result;
    }
}

template <typename Function, typename... Arguments>
auto bound_map_graph(const Function &function) {
    using Bound = BoundMapGraphFunction<Function, Arguments...>;
    auto signature = make_graph_signature<Bound>(util::symbol{"map_function"});
    return GraphTemplate{std::move(signature), Bound{function}};
}

template <std::size_t Index, bool KeyInserted, typename Arguments, typename Function,
          typename Callback, typename Key, typename... Parameters>
void dispatch_map_graph_function(const Function &function, Callback &callback,
                                 std::optional<std::size_t> selected_key_set,
                                 std::optional<std::size_t> key_parameter) {
    if constexpr (!KeyInserted) {
        if (key_parameter && *key_parameter == sizeof...(Parameters)) {
            dispatch_map_graph_function<Index, true, Arguments, Function, Callback, Key, Parameters..., Key>(
                function, callback, selected_key_set, key_parameter);
            return;
        }
    }

    if constexpr (Index == std::tuple_size_v<Arguments>) {
        if (key_parameter && !KeyInserted) {
            throw WiringError{"__key_arg__ selects a graph parameter that is out of range"};
        }
        if constexpr (std::invocable<const Function &, Parameters...>) {
            callback(bound_map_graph<Function, Parameters...>(function));
        } else {
            throw WiringError{"the map graph function does not accept the per-key arguments"};
        }
        return;
    } else {
        using Supplied = std::tuple_element_t<Index, Arguments>;
        if constexpr (map_key_set_candidate<Supplied>) {
            if (selected_key_set == Index) {
                dispatch_map_graph_function<Index + 1, KeyInserted, Arguments, Function, Callback, Key,
                                            Parameters...>(
                    function, callback, selected_key_set, key_parameter);
                return;
            }
        }

        using Parameter = map_graph_function_parameter_t<Supplied>;
        dispatch_map_graph_function<Index + 1, KeyInserted, Arguments, Function, Callback, Key,
                                    Parameters..., Parameter>(function, callback, selected_key_set, key_parameter);
    }
}

template <typename SelectOverload, typename Graph, typename Call>
wp<> invoke_preprocessed_map(SelectOverload &&select_overload, const Graph &function, Call &call,
                             bool preserve_named_arguments) {
    auto graph_argument = build_map_plan(
        function, map_supplied_arguments(call.positional), map_supplied_arguments(call.named),
        call.key_arg, call.key_set_arg, preserve_named_arguments);
    return select_map_overload(std::forward<SelectOverload>(select_overload), graph_argument,
                               call.positional, call.named, call.options);
}

template <typename SelectOverload, typename Graph, typename... Arguments>
auto invoke_map_graph(SelectOverload &&select_overload, const Graph &function,
                      Arguments &&...arguments) {
    auto call = preprocess_map_graph_call(std::forward<Arguments>(arguments)...);
    return invoke_preprocessed_map(std::forward<SelectOverload>(select_overload), function, call, true);
}

template <typename SelectOverload, typename Function, typename... Arguments>
auto invoke_map_graph_function(SelectOverload &&select_overload, Function &&function,
                               Arguments &&...arguments) {
    if constexpr (!map_call_traits<Arguments...>::valid) {
        validate_map_call<Arguments...>();
    } else {
        using StoredFunction = std::decay_t<Function>;
        auto stored_function = StoredFunction{std::forward<Function>(function)};
        auto call = preprocess_map_call(std::forward<Arguments>(arguments)...);
        auto positional = map_supplied_arguments(call.positional);
        auto named = map_supplied_arguments(call.named);
        const auto selected_key_set = selected_map_argument_index(call.key_set_arg, positional, named);
        using Supplied = decltype(std::tuple_cat(call.positional, call.named));
        using Key = typename decltype(call)::inserted_key_type;
        constexpr auto explicit_key = map_call_traits<Arguments...>::key_argument_count != 0;
        constexpr auto omit_sole_set =
            map_call_traits<Arguments...>::key_set_candidate_count == 1 &&
            map_call_traits<Arguments...>::key_set_argument_count == 0;
        constexpr auto no_key_shape =
            map_graph_function_result<0, false, false, false, omit_sole_set, Supplied, StoredFunction, Key>();
        constexpr auto leading_key_shape =
            map_graph_function_result<0, false, true, false, omit_sole_set, Supplied, StoredFunction, Key, Key>();
        constexpr auto no_key_invocable = no_key_shape != MapGraphFunctionResult::invalid;
        constexpr auto leading_key_invocable = leading_key_shape != MapGraphFunctionResult::invalid;

        if constexpr (!explicit_key && no_key_invocable && leading_key_invocable) {
            throw WiringError{
                "a map graph function accepts arguments with and without a leading key; provide __key_arg__"};
        } else {
            constexpr auto inferred_key =
                !explicit_key && !no_key_invocable && leading_key_invocable;
            constexpr auto result_shape = [] {
                if constexpr (explicit_key) {
                    return map_graph_function_result<0, true, false, false, omit_sole_set,
                                                     Supplied, StoredFunction, Key>();
                } else if constexpr (inferred_key) {
                    return leading_key_shape;
                } else {
                    return no_key_shape;
                }
            }();
            static_assert(result_shape != MapGraphFunctionResult::invalid,
                          "the map graph function does not accept the per-key arguments");
            static_assert(result_shape != MapGraphFunctionResult::inconsistent,
                          "map graph function argument sequences must have one output shape");

            if constexpr (inferred_key) {
                call.key_arg.emplace(std::size_t{0});
            }
            const auto key_parameter = [&] {
                if (!call.key_arg) {
                    return std::optional<std::size_t>{};
                }
                if (const auto index = call.key_arg->index()) {
                    return std::optional{*index};
                }
                throw WiringError{"a map graph function requires an indexed __key_arg__ selector"};
            }();

            auto result = wp<>{};
            auto materialize = [&](const auto &bound) {
                result = invoke_preprocessed_map(select_overload, bound, call, false);
            };
            dispatch_map_graph_function<0, false, Supplied, StoredFunction, decltype(materialize), Key>(
                stored_function, materialize, selected_key_set, key_parameter);
            return result;
        }
    }
}

struct map_operator_implementation {
    template <typename SelectOverload, typename Graph, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate> &&
                 requires { typename std::remove_cvref_t<Graph>::result_type; }
    static auto invoke(SelectOverload &&select_overload, const Graph &function,
                       Arguments &&...arguments) {
        return invoke_map_graph(std::forward<SelectOverload>(select_overload), function,
                                std::forward<Arguments>(arguments)...);
    }

    template <typename SelectOverload, typename Function, typename... Arguments>
        requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
    static auto invoke(SelectOverload &&select_overload, Function &&function,
                       Arguments &&...arguments) {
        return invoke_map_graph_function(std::forward<SelectOverload>(select_overload),
                                         std::forward<Function>(function),
                                         std::forward<Arguments>(arguments)...);
    }
};

} // namespace bg::details
