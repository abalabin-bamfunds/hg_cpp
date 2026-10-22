#pragma once

#include <bg/graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/switch_argument.hpp>
#include <bg/switch/details/default_switch_wiring.hpp>
#include <bg/util/named_argument.hpp>

#include <concepts>
#include <functional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

template <typename Key, typename Function> struct switch_case {
    Key key;
    Function function;
};

template <typename... Case>
class switch_cases {
  public:
    explicit switch_cases(Case... values) : values_{std::move(values)...} {}

    const std::tuple<Case...> &values() const noexcept { return values_; }

  private:
    std::tuple<Case...> values_;
};

template <typename T>
inline constexpr bool is_switch_cases_v = requires(const std::remove_cvref_t<T> &value) {
    value.values();
};

template <typename T> struct is_switch_key_tuple : std::false_type {};

template <typename... Key>
struct is_switch_key_tuple<std::tuple<Key...>> : std::true_type {};

template <typename T>
inline constexpr bool is_switch_key_tuple_v =
    is_switch_key_tuple<std::remove_cvref_t<T>>::value;

template <typename Function, typename... Arguments> struct BoundSwitchGraphFunction {
    Function function;

    decltype(auto) operator()(Arguments... arguments) const {
        return std::invoke(function, std::move(arguments)...);
    }
};

template <typename Argument> struct switch_callable_argument {
    using type = std::remove_cvref_t<Argument>;
};

template <util::compile_time_string Name, typename Value>
struct switch_callable_argument<named_argument<Name, Value>> {
    using type = std::remove_cvref_t<Value>;
};

template <typename Argument>
using switch_callable_argument_t = typename switch_callable_argument<std::remove_cvref_t<Argument>>::type;

template <typename Function, typename... Arguments>
auto bound_switch_graph(const Function &function) {
    using Bound = BoundSwitchGraphFunction<Function, switch_callable_argument_t<Arguments>...>;
    auto signature = make_graph_signature<Bound>(util::symbol{"switch_function"});
    return GraphTemplate{std::move(signature), Bound{function}};
}

GraphArgument build_switch_graph_argument(
    WiringPoint key, std::span<const WiringArgument> arguments,
    std::span<const PreparedSwitchCase> cases);

template <typename Argument> WiringArgument switch_supplied_argument(Argument &argument) {
    return WiringArgument{argument};
}

template <typename Tuple, std::size_t... Index>
std::vector<WiringArgument> switch_supplied_arguments(Tuple &arguments,
                                                      std::index_sequence<Index...>) {
    auto result = std::vector<WiringArgument>{};
    result.reserve(sizeof...(Index));
    (result.emplace_back(switch_supplied_argument(std::get<Index>(arguments))), ...);
    return result;
}

template <typename Tuple>
std::vector<WiringArgument> switch_supplied_arguments(Tuple &arguments) {
    return switch_supplied_arguments(
        arguments, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename SelectOverload>
auto select_switch_overload(SelectOverload &&select_overload, WiringArgument key,
                            GraphArgument prepared, std::span<const WiringArgument> arguments) {
    auto overload_arguments = std::vector<WiringArgument>{};
    overload_arguments.reserve(arguments.size() + 2);
    overload_arguments.push_back(std::move(key));
    overload_arguments.emplace_back(std::move(prepared));
    for (const auto &argument : arguments) {
        if (argument.is_time_series()) {
            overload_arguments.push_back(WiringArgument::borrow(argument));
        }
    }
    return std::invoke(std::forward<SelectOverload>(select_overload),
                       std::span<const WiringArgument>{overload_arguments});
}

template <typename Function, typename Arguments, std::size_t... Index>
auto bound_switch_graph_for_arguments(const Function &function, std::index_sequence<Index...>) {
    return bound_switch_graph<Function, std::tuple_element_t<Index, Arguments>...>(function);
}

template <typename Function, typename Arguments>
auto bound_switch_graph_for_arguments(const Function &function) {
    return bound_switch_graph_for_arguments<Function, Arguments>(
        function, std::make_index_sequence<std::tuple_size_v<Arguments>>{});
}

template <typename SelectOverload, typename Key, typename Cases, typename... Arguments>
auto invoke_switch(SelectOverload &&select_overload, Key key, const Cases &case_values,
                   Arguments &&...arguments) {
    static_assert(is_typed_wiring_point<Key>, "switch_ key must be a typed wiring point");
    static_assert(is_switch_cases_v<Cases>, "switch_ requires cases{case_(...), ...}");

    auto supplied_tuple = std::tuple<std::decay_t<Arguments>...>{
        std::forward<Arguments>(arguments)...};
    auto supplied = switch_supplied_arguments(supplied_tuple);
    auto prepared_cases = std::vector<PreparedSwitchCase>{};
    auto owned_graphs = std::vector<std::shared_ptr<const WiringTemplate>>{};

    std::apply(
        [&](const auto &...item) {
            if constexpr (sizeof...(item) == 0) {
                return;
            } else {
                const auto append = [&](const auto &entry) {
                    using Function = std::remove_cvref_t<decltype(entry.function)>;
                    const WiringTemplate *function{};
                    if constexpr (std::derived_from<Function, WiringTemplate>) {
                        function = &entry.function;
                    } else {
                        auto graph = bound_switch_graph_for_arguments<Function,
                                                                      decltype(supplied_tuple)>(
                            entry.function);
                        auto owned = std::make_shared<decltype(graph)>(std::move(graph));
                        function = owned.get();
                        owned_graphs.push_back(std::move(owned));
                    }

                    auto prepared = PreparedSwitchCase{
                        .keys = {},
                        .default_case = false,
                        .function = function,
                        .preserve_argument_names = std::derived_from<Function, WiringTemplate>,
                    };
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(entry.key)>,
                                               default_switch_case_t>) {
                        prepared.default_case = true;
                    } else if constexpr (is_switch_key_tuple_v<decltype(entry.key)>) {
                        std::apply(
                            [&](const auto &...key) {
                                (prepared.keys.emplace_back(key), ...);
                            },
                            entry.key);
                    } else {
                        prepared.keys.emplace_back(entry.key);
                    }
                    prepared_cases.push_back(std::move(prepared));
                };
                (append(item), ...);
            }
        },
        case_values.values());

    auto prepared = build_switch_graph_argument(
        static_cast<WiringPoint>(key), supplied, prepared_cases);

    return select_switch_overload(std::forward<SelectOverload>(select_overload), WiringArgument{key},
                                  std::move(prepared), supplied);
}

struct switch_operator_implementation {
    template <typename SelectOverload, typename Key>
    static auto invoke(const TypeMap &, SelectOverload &&select_overload, Key &&key,
                       std::span<const SwitchCaseArgument> cases,
                       std::span<const WiringArgument> arguments) {
        static_assert(wiring_point_argument<Key> || std::same_as<std::remove_cvref_t<Key>, WiringArgument>,
                      "switch_ key must be a wiring point or a time-series wiring argument");
        auto key_argument = WiringArgument{std::forward<Key>(key)};
        auto key_point = key_argument.time_series();
        auto prepared_cases = std::vector<PreparedSwitchCase>{};
        prepared_cases.reserve(cases.size());
        for (const auto &item : cases) {
            if (item.function == nullptr) {
                throw WiringError{"a switch case requires a graph"};
            }
            prepared_cases.push_back({item.keys, item.default_case, item.function, true});
        }

        auto prepared = build_switch_graph_argument(key_point, arguments, prepared_cases);
        return select_switch_overload(std::forward<SelectOverload>(select_overload),
                                      WiringArgument::borrow(key_argument), std::move(prepared), arguments);
    }

    template <typename SelectOverload, typename Key, typename Cases, typename... Arguments>
    static auto invoke(const TypeMap &, SelectOverload &&select_overload, Key key, const Cases &case_values,
                       Arguments &&...arguments) {
        return invoke_switch(std::forward<SelectOverload>(select_overload), key, case_values,
                             std::forward<Arguments>(arguments)...);
    }
};

} // namespace details

template <typename Key, typename Function>
auto case_(Key &&key, Function &&function) {
    return details::switch_case<std::decay_t<Key>, std::decay_t<Function>>{
        std::forward<Key>(key), std::forward<Function>(function)};
}

template <typename... Case>
class cases : public details::switch_cases<std::decay_t<Case>...> {
  public:
    explicit cases(Case &&...values)
        : details::switch_cases<std::decay_t<Case>...>{std::forward<Case>(values)...} {}
};

template <typename... Case> cases(Case &&...) -> cases<Case...>;

} // namespace bg
