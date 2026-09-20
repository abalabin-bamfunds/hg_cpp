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

template <typename Operation, typename Positional, typename Named, std::size_t... P, std::size_t... N>
decltype(auto) invoke_map_operator(Operation &operation, const GraphArgument &function,
                                   Positional &positional, Named &named,
                                   std::index_sequence<P...>, std::index_sequence<N...>) {
    return static_cast<const WiringTemplate &>(operation)(
        function, unwrap_map_argument(std::get<P>(positional))...,
        unwrap_map_argument(std::get<N>(named))...);
}

template <typename Operation, typename Positional, typename Named>
decltype(auto) invoke_map_operator(Operation &operation, const GraphArgument &function,
                                   Positional &positional, Named &named) {
    return invoke_map_operator(operation, function, positional, named,
                               std::make_index_sequence<std::tuple_size_v<Positional>>{},
                               std::make_index_sequence<std::tuple_size_v<Named>>{});
}

template <typename T> struct raw_map_argument {
    using type = typename map_argument_traits_t<T>::per_key_type;
};

template <util::compile_time_string Name, typename T> struct raw_map_argument<named_argument<Name, T>> {
    using type = typename map_argument_traits_t<T>::per_key_type;
};

template <typename T> using raw_map_argument_t = typename raw_map_argument<std::remove_cvref_t<T>>::type;

template <typename Function, typename... Arguments> struct BoundMapFunction {
    Function function;

    decltype(auto) operator()(Arguments... arguments) const {
        return std::invoke(function, std::move(arguments)...);
    }
};

enum class RawMapResultShape : unsigned char {
    invalid,
    sink,
    value,
    inconsistent,
};

consteval RawMapResultShape combine_raw_map_shapes(RawMapResultShape left, RawMapResultShape right) {
    if (left == RawMapResultShape::invalid) {
        return right;
    }
    if (right == RawMapResultShape::invalid) {
        return left;
    }
    return left == right ? left : RawMapResultShape::inconsistent;
}

template <typename Function, typename... Arguments> consteval RawMapResultShape raw_invocation_shape() {
    if constexpr (!std::invocable<const Function &, Arguments...>) {
        return RawMapResultShape::invalid;
    } else {
        using Result = std::invoke_result_t<const Function &, Arguments...>;
        if constexpr (std::same_as<Result, void>) {
            return RawMapResultShape::sink;
        } else if constexpr (is_typed_wiring_point<Result>) {
            return RawMapResultShape::value;
        } else {
            return RawMapResultShape::inconsistent;
        }
    }
}

template <std::size_t Index, bool HasKey, bool KeyInserted, bool SetOmitted, bool OmitSoleSet,
          typename Supplied, typename Function, typename Key, typename... Parameters>
consteval RawMapResultShape raw_map_result_shape() {
    auto result = RawMapResultShape::invalid;
    if constexpr (HasKey && !KeyInserted) {
        result = combine_raw_map_shapes(
            result,
            raw_map_result_shape<Index, HasKey, true, SetOmitted, OmitSoleSet, Supplied, Function, Key,
                                 Parameters..., Key>());
    }

    if constexpr (Index == std::tuple_size_v<Supplied>) {
        if constexpr (!HasKey || KeyInserted) {
            result = combine_raw_map_shapes(result, raw_invocation_shape<Function, Parameters...>());
        }
        return result;
    } else {
        using Argument = std::tuple_element_t<Index, Supplied>;
        if constexpr (!SetOmitted && map_key_set_candidate<Argument>) {
            result = combine_raw_map_shapes(
                result,
                raw_map_result_shape<Index + 1, HasKey, KeyInserted, true, OmitSoleSet, Supplied, Function, Key,
                                     Parameters...>());
        }
        if constexpr (!OmitSoleSet || SetOmitted || !map_key_set_candidate<Argument>) {
            using Parameter = raw_map_argument_t<Argument>;
            result = combine_raw_map_shapes(
                result,
                raw_map_result_shape<Index + 1, HasKey, KeyInserted, SetOmitted, OmitSoleSet,
                                     Supplied, Function, Key, Parameters..., Parameter>());
        }
        return result;
    }
}

template <typename Function, typename... Arguments>
auto bound_map_graph(const Function &function) {
    using Bound = BoundMapFunction<Function, Arguments...>;
    auto signature = make_graph_signature<Bound>(util::symbol{"map_function"});
    return GraphTemplate{std::move(signature), Bound{function}};
}

template <std::size_t Index, bool KeyInserted, typename Arguments, typename Function,
          typename Callback, typename Key, typename... Parameters>
void dispatch_raw_map_graph(const Function &function, Callback &callback,
                            std::optional<std::size_t> selected_key_set,
                            std::optional<std::size_t> key_parameter) {
    if constexpr (!KeyInserted) {
        if (key_parameter && *key_parameter == sizeof...(Parameters)) {
            dispatch_raw_map_graph<Index, true, Arguments, Function, Callback, Key, Parameters..., Key>(
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
            throw WiringError{"the map callable does not accept the derived per-key arguments"};
        }
        return;
    } else {
        using Supplied = std::tuple_element_t<Index, Arguments>;
        if constexpr (map_key_set_candidate<Supplied>) {
            if (selected_key_set == Index) {
                dispatch_raw_map_graph<Index + 1, KeyInserted, Arguments, Function, Callback, Key, Parameters...>(
                    function, callback, selected_key_set, key_parameter);
                return;
            }
        }

        using Parameter = raw_map_argument_t<Supplied>;
        dispatch_raw_map_graph<Index + 1, KeyInserted, Arguments, Function, Callback, Key,
                               Parameters..., Parameter>(function, callback, selected_key_set, key_parameter);
    }
}

template <typename Operation, typename Graph, typename Call>
std::optional<WiringPoint> invoke_preprocessed_map(Operation &operation, const Graph &function,
                                                   Call &call, bool preserve_named_arguments) {
    auto graph_argument = build_map_plan(
        function, map_supplied_arguments(call.positional), map_supplied_arguments(call.named),
        cpp_scalar_type<typename Call::key_type>(), call.key_arg, call.key_set_arg,
        preserve_named_arguments);
    return invoke_map_operator(operation, graph_argument, call.positional, call.named);
}

template <typename Operation, typename Graph, typename... Arguments>
auto invoke_map_graph(Operation &operation, const Graph &function, Arguments &&...arguments) {
    using Result = typename std::remove_cvref_t<Graph>::result_type;
    auto call = preprocess_map_call(std::forward<Arguments>(arguments)...);
    const auto result = invoke_preprocessed_map(operation, function, call, true);

    if constexpr (std::same_as<Result, void>) {
        if (result) {
            throw WiringError{"a sink map overload produced an output"};
        }
    } else {
        if (!result) {
            throw WiringError{"a value map overload produced no output"};
        }
        return wp<>{*result};
    }
}

template <typename Operation, typename Function, typename... Arguments>
auto invoke_raw_map_graph(Operation &operation, Function &&function, Arguments &&...arguments) {
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
        raw_map_result_shape<0, false, false, false, omit_sole_set, Supplied, StoredFunction, Key>();
    constexpr auto leading_key_shape =
        raw_map_result_shape<0, false, true, false, omit_sole_set, Supplied, StoredFunction, Key, Key>();
    constexpr auto no_key_invocable = no_key_shape != RawMapResultShape::invalid;
    constexpr auto leading_key_invocable = leading_key_shape != RawMapResultShape::invalid;

    if constexpr (!explicit_key && no_key_invocable && leading_key_invocable) {
        throw WiringError{
            "a raw map callable accepts arguments with and without a leading key; provide __key_arg__"};
    } else {
        constexpr auto inferred_key =
            !explicit_key && !no_key_invocable && leading_key_invocable;
        constexpr auto result_shape = [] {
            if constexpr (explicit_key) {
                return raw_map_result_shape<0, true, false, false, omit_sole_set,
                                            Supplied, StoredFunction, Key>();
            } else if constexpr (inferred_key) {
                return leading_key_shape;
            } else {
                return no_key_shape;
            }
        }();
        static_assert(result_shape != RawMapResultShape::invalid,
                      "the raw map callable does not accept any derived per-key argument sequence");
        static_assert(result_shape != RawMapResultShape::inconsistent,
                      "raw map callable argument sequences must have one output shape");

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
            throw WiringError{"a raw map callable requires an indexed __key_arg__ selector"};
        }();

        auto result = std::optional<WiringPoint>{};
        auto invoke = [&](const auto &bound) {
            result = invoke_preprocessed_map(operation, bound, call, false);
        };
        dispatch_raw_map_graph<0, false, Supplied, StoredFunction, decltype(invoke), Key>(
            stored_function, invoke, selected_key_set, key_parameter);
        if constexpr (result_shape == RawMapResultShape::sink) {
            if (result) {
                throw WiringError{"a sink map overload produced an output"};
            }
        } else {
            if (!result) {
                throw WiringError{"a value map overload produced no output"};
            }
            return wp<>{*result};
        }
    }
}

} // namespace bg::details
