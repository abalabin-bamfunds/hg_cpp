#pragma once

#include <bg/graph.hpp>
#include <bg/map/details/default_map_wiring.hpp>
#include <bg/map/details/map_call.hpp>
#include <bg/operator.hpp>

#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
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

template <typename Argument> MapArgument map_supplied_argument(Argument &argument) {
    if constexpr (is_named_argument_v<Argument>) {
        auto unwrapped = unwrap_map_argument(argument);
        return {WiringArgument{unwrapped}, is_pass_through_argument_v<decltype(argument.value)>};
    } else {
        return {WiringArgument{unwrap_map_argument(argument)}, is_pass_through_argument_v<Argument>};
    }
}

template <typename Tuple, std::size_t... Index>
std::vector<MapArgument> map_supplied_arguments(Tuple &tuple, std::index_sequence<Index...>) {
    auto result = std::vector<MapArgument>{};
    result.reserve(sizeof...(Index));
    (result.push_back(map_supplied_argument(std::get<Index>(tuple))), ...);
    return result;
}

template <typename Tuple> std::vector<MapArgument> map_supplied_arguments(Tuple &tuple) {
    return map_supplied_arguments(tuple, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename SelectOverload, typename Positional, typename Named, typename Options, std::size_t... P,
          std::size_t... N, std::size_t... O>
decltype(auto) select_map_overload(SelectOverload &&select_overload, const GraphArgument &function,
                                   Positional &positional, Named &named, Options &options, std::index_sequence<P...>,
                                   std::index_sequence<N...>, std::index_sequence<O...>) {
    return std::invoke(std::forward<SelectOverload>(select_overload), function,
                       unwrap_map_argument(std::get<P>(positional))..., unwrap_map_argument(std::get<N>(named))...,
                       std::get<O>(options)...);
}

template <typename SelectOverload, typename Positional, typename Named, typename Options>
decltype(auto) select_map_overload(SelectOverload &&select_overload, const GraphArgument &function,
                                   Positional &positional, Named &named, Options &options) {
    return select_map_overload(std::forward<SelectOverload>(select_overload), function, positional, named, options,
                               std::make_index_sequence<std::tuple_size_v<Positional>>{},
                               std::make_index_sequence<std::tuple_size_v<Named>>{},
                               std::make_index_sequence<std::tuple_size_v<Options>>{});
}

template <typename T> using map_graph_function_parameter_t = typename map_callable_argument_t<T>::parameter_type;

template <typename Function> inline constexpr bool map_function_signature_available = [] {
    if constexpr (std::is_pointer_v<Function>) {
        return true;
    } else if constexpr (std::is_class_v<Function>) {
        return requires { &Function::operator(); };
    } else {
        return false;
    }
}();

template <typename Function, bool = map_function_signature_available<Function>> struct map_function_signature {
    static constexpr bool available = false;
    static constexpr std::size_t argument_count = 0;
};

template <typename Function> struct map_function_signature<Function, true> {
    using callable_type = callable<Function>;

    static constexpr bool available = true;
    static constexpr std::size_t argument_count = callable_type::argument_count;

    template <std::size_t Index> using argument_type = typename callable_type::template argument_type<Index>;
};

template <typename Function, std::size_t Index, typename Supplied,
          bool HasDeclaredParameter = map_function_signature<Function>::available &&
                                      Index < map_function_signature<Function>::argument_count>
struct map_function_parameter {
    using type = map_graph_function_parameter_t<Supplied>;
};

template <typename Function, std::size_t Index, typename Supplied>
struct map_function_parameter<Function, Index, Supplied, true> {
    using type = std::remove_cvref_t<typename map_function_signature<Function>::template argument_type<Index>>;
};

template <typename Function, std::size_t Index, typename Supplied>
using map_function_parameter_t = typename map_function_parameter<Function, Index, Supplied>::type;

template <typename Function, typename... Arguments> struct BoundMapGraphFunction {
    Function function;

    decltype(auto) operator()(Arguments... arguments) const { return std::invoke(function, std::move(arguments)...); }
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

template <std::size_t Index, bool HasKey, bool KeyInserted, bool SetOmitted, bool OmitSoleSet, typename Supplied,
          typename Function, typename Key, typename... Parameters>
consteval MapGraphFunctionResult map_graph_function_result() {
    auto result = MapGraphFunctionResult::invalid;
    if constexpr (HasKey && !KeyInserted) {
        result = combine_map_graph_function_results(
            result, map_graph_function_result<Index, HasKey, true, SetOmitted, OmitSoleSet, Supplied, Function, Key,
                                              Parameters..., Key>());
    }

    if constexpr (Index == std::tuple_size_v<Supplied>) {
        if constexpr (!HasKey || KeyInserted) {
            if constexpr (!map_function_signature<Function>::available ||
                          sizeof...(Parameters) == map_function_signature<Function>::argument_count) {
                result = combine_map_graph_function_results(result, map_graph_function_result<Function, Parameters...>());
            }
        }
        return result;
    } else {
        using Argument = std::tuple_element_t<Index, Supplied>;
        if constexpr (!SetOmitted && map_callable_key_set<Argument>) {
            result = combine_map_graph_function_results(
                result, map_graph_function_result<Index + 1, HasKey, KeyInserted, true, OmitSoleSet, Supplied, Function,
                                                  Key, Parameters...>());
        }
        if constexpr (!OmitSoleSet || SetOmitted || !map_callable_key_set<Argument>) {
            if constexpr (!map_function_signature<Function>::available ||
                          sizeof...(Parameters) < map_function_signature<Function>::argument_count) {
                using Parameter = map_function_parameter_t<Function, sizeof...(Parameters), Argument>;
                result = combine_map_graph_function_results(
                    result, map_graph_function_result<Index + 1, HasKey, KeyInserted, SetOmitted, OmitSoleSet,
                                                      Supplied, Function, Key, Parameters..., Parameter>());
            }
        }
        return result;
    }
}

template <typename Function, typename... Arguments> auto bound_map_graph(const Function &function) {
    using Bound = BoundMapGraphFunction<Function, Arguments...>;
    auto signature = make_graph_signature<Bound>(util::symbol{"map_function"});
    return GraphTemplate{std::move(signature), Bound{function}};
}

template <std::size_t Index, bool KeyInserted, typename Arguments, typename Function, typename Callback, typename Key,
          typename... Parameters>
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
            throw WiringError{"the inner graph function does not accept the arguments for one key"};
        }
        return;
    } else {
        using Supplied = std::tuple_element_t<Index, Arguments>;
        if constexpr (map_callable_key_set<Supplied>) {
            if (selected_key_set == Index) {
                dispatch_map_graph_function<Index + 1, KeyInserted, Arguments, Function, Callback, Key, Parameters...>(
                    function, callback, selected_key_set, key_parameter);
                return;
            }
        }

        if constexpr (!map_function_signature<Function>::available ||
                      sizeof...(Parameters) < map_function_signature<Function>::argument_count) {
            using Parameter = map_function_parameter_t<Function, sizeof...(Parameters), Supplied>;
            dispatch_map_graph_function<Index + 1, KeyInserted, Arguments, Function, Callback, Key, Parameters...,
                                        Parameter>(function, callback, selected_key_set, key_parameter);
        } else {
            throw WiringError{"the inner graph function does not accept the arguments for one key"};
        }
    }
}

template <typename Call> ResolvedMapCall resolve_map_call(std::string_view operation, Call &call) {
    auto arguments = map_supplied_arguments(call.positional);
    auto named = map_supplied_arguments(call.named);
    auto options = map_supplied_arguments(call.options);
    arguments.reserve(arguments.size() + named.size() + options.size());
    arguments.insert(arguments.end(), std::make_move_iterator(named.begin()), std::make_move_iterator(named.end()));
    arguments.insert(arguments.end(), std::make_move_iterator(options.begin()), std::make_move_iterator(options.end()));
    return resolve_map_call(operation, std::span<const MapArgument>{arguments});
}

struct map_graph_argument_builder {
    static constexpr auto name = "map";

    GraphArgument operator()(const WiringTemplate &function, ResolvedMapCall call,
                             bool preserve_named_arguments, const TypeMap &type_map) const {
        return build_map_graph_argument(function, std::move(call), preserve_named_arguments, type_map);
    }
};

template <typename SelectOverload, typename Graph, typename Call, typename Builder>
wp<> invoke_preprocessed_map(SelectOverload &&select_overload, const Graph &function, Call &call,
                             ResolvedMapCall resolved_call, bool preserve_named_arguments, Builder builder,
                             const TypeMap &type_map) {
    auto graph_argument = builder(function, std::move(resolved_call), preserve_named_arguments, type_map);
    return select_map_overload(std::forward<SelectOverload>(select_overload), graph_argument, call.positional,
                               call.named, call.options);
}

template <typename SelectOverload, typename Graph, typename Builder, typename... Arguments>
auto invoke_map(const TypeMap &type_map, SelectOverload &&select_overload, const Graph &function, Builder builder,
                Arguments &&...arguments) {
    auto call = group_map_call_arguments(std::forward<Arguments>(arguments)...);
    auto resolved_call = resolve_map_call(Builder::name, call);
    return invoke_preprocessed_map(std::forward<SelectOverload>(select_overload), function, call,
                                   std::move(resolved_call), true, builder, type_map);
}

template <typename SelectOverload, typename Builder>
auto invoke_runtime_map(const TypeMap &type_map, SelectOverload &&select_overload, const WiringTemplate &function,
                        std::span<const MapArgument> arguments, Builder builder) {
    auto call = resolve_map_call(Builder::name, arguments);
    auto graph_argument = builder(function, call, true, type_map);

    auto overload_arguments = std::vector<WiringArgument>{};
    overload_arguments.reserve(1 + call.positional.size() + call.named.size() + call.options.size());
    overload_arguments.emplace_back(graph_argument);
    for (const auto &argument : call.positional) {
        overload_arguments.push_back(argument.value);
    }
    for (const auto &argument : call.named) {
        overload_arguments.push_back(argument.value);
    }
    for (const auto &option : call.options) {
        overload_arguments.push_back(option);
    }

    return std::invoke(std::forward<SelectOverload>(select_overload),
                       std::span<const WiringArgument>{overload_arguments});
}

template <typename SelectOverload, typename Function, typename Builder, typename... Arguments>
auto invoke_map_function(const TypeMap &type_map, SelectOverload &&select_overload, Function &&function,
                         Builder builder,
                         Arguments &&...arguments) {
    using KeyScalar = typename first_map_callable_key<Arguments...>::type;
    if constexpr (std::same_as<KeyScalar, void>) {
        auto call = group_map_call_arguments(std::forward<Arguments>(arguments)...);
        (void)resolve_map_call(Builder::name, call);
        throw WiringError{std::string{"a "} + Builder::name +
                          " graph function requires typed TSD or TSS arguments; use a graph template with wp<>"};
    } else {
        using StoredFunction = std::decay_t<Function>;
        auto stored_function = StoredFunction{std::forward<Function>(function)};
        auto call = group_map_call_arguments(std::forward<Arguments>(arguments)...);
        auto resolved_call = resolve_map_call(Builder::name, call);
        const auto selected_key_set = resolved_call.selected_key_set;
        using Supplied = decltype(std::tuple_cat(call.positional, call.named));
        using Key = map_key_parameter_t<KeyScalar>;
        constexpr auto explicit_key = named_argument_count<map_key_argument_name, Arguments...> != 0;
        constexpr auto key_set_count =
            (std::size_t{} + ... + static_cast<std::size_t>(map_callable_key_set<Arguments>));
        constexpr auto omit_sole_set =
            key_set_count == 1 && named_argument_count<map_key_set_argument_name, Arguments...> == 0;
        constexpr auto no_key_shape =
            map_graph_function_result<0, false, false, false, omit_sole_set, Supplied, StoredFunction, Key>();
        constexpr auto leading_key_shape =
            map_graph_function_result<0, false, true, false, omit_sole_set, Supplied, StoredFunction, Key, Key>();
        constexpr auto no_key_invocable = no_key_shape != MapGraphFunctionResult::invalid;
        constexpr auto leading_key_invocable = leading_key_shape != MapGraphFunctionResult::invalid;

        if constexpr (!explicit_key && no_key_invocable && leading_key_invocable) {
            throw WiringError{std::string{"a "} + Builder::name +
                              " graph function accepts arguments with and without a leading key; provide __key_arg__"};
        } else {
            constexpr auto inferred_key = !explicit_key && !no_key_invocable && leading_key_invocable;
            constexpr auto result_shape = [] {
                if constexpr (explicit_key) {
                    return map_graph_function_result<0, true, false, false, omit_sole_set, Supplied, StoredFunction,
                                                     Key>();
                } else if constexpr (inferred_key) {
                    return leading_key_shape;
                } else {
                    return no_key_shape;
                }
            }();
            static_assert(result_shape != MapGraphFunctionResult::invalid,
                          "the inner graph function does not accept the arguments for one key");
            static_assert(result_shape != MapGraphFunctionResult::inconsistent,
                          "inner graph function argument sequences must have one output shape");

            if constexpr (inferred_key) {
                resolved_call.key_argument.emplace(std::size_t{0});
            }
            const auto key_parameter = [&] {
                if (!resolved_call.key_argument) {
                    return std::optional<std::size_t>{};
                }
                if (const auto index = resolved_call.key_argument->index()) {
                    return std::optional{*index};
                }
                throw WiringError{"an inner graph function requires an indexed __key_arg__ selector"};
            }();

            auto result = wp<>{};
            auto materialize = [&](const auto &bound) {
                result =
                    invoke_preprocessed_map(select_overload, bound, call, std::move(resolved_call), false, builder,
                                            type_map);
            };
            dispatch_map_graph_function<0, false, Supplied, StoredFunction, decltype(materialize), Key>(
                stored_function, materialize, selected_key_set, key_parameter);
            return result;
        }
    }
}

struct map_operator_implementation {
    template <typename SelectOverload, typename Graph, typename Argument, std::size_t Extent>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate> &&
                 std::same_as<std::remove_const_t<Argument>, MapArgument>
    static auto invoke(const TypeMap &type_map, SelectOverload &&select_overload, const Graph &function,
                       std::span<Argument, Extent> arguments) {
        return invoke_runtime_map(type_map, std::forward<SelectOverload>(select_overload), function, arguments,
                                  map_graph_argument_builder{});
    }

    template <typename SelectOverload, typename Graph, typename... Arguments>
        requires std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate>
    static auto invoke(const TypeMap &type_map, SelectOverload &&select_overload, const Graph &function,
                       Arguments &&...arguments) {
        return invoke_map(type_map, std::forward<SelectOverload>(select_overload), function,
                          map_graph_argument_builder{}, std::forward<Arguments>(arguments)...);
    }

    template <typename SelectOverload, typename Function, typename... Arguments>
        requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
    static auto invoke(const TypeMap &type_map, SelectOverload &&select_overload, Function &&function,
                       Arguments &&...arguments) {
        return invoke_map_function(type_map, std::forward<SelectOverload>(select_overload),
                                   std::forward<Function>(function), map_graph_argument_builder{},
                                   std::forward<Arguments>(arguments)...);
    }
};

} // namespace bg::details
