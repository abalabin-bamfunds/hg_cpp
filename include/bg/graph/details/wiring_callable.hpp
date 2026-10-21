#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/util/symbol.hpp>

#include <concepts>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T> struct callable;

template <typename Result, typename... Arguments> struct callable<Result(Arguments...)> {
    using result_type = Result;
    static constexpr std::size_t argument_count = sizeof...(Arguments);

    template <std::size_t Index> using argument_type = std::tuple_element_t<Index, std::tuple<Arguments...>>;
};

template <typename Result, typename... Arguments>
struct callable<Result (*)(Arguments...)> : callable<Result(Arguments...)> {};

template <typename Result, typename... Arguments>
struct callable<Result (*)(Arguments...) noexcept> : callable<Result(Arguments...)> {};

template <typename Class, typename Result, typename... Arguments>
struct callable<Result (Class::*)(Arguments...) const> : callable<Result(Arguments...)> {};

template <typename Class, typename Result, typename... Arguments>
struct callable<Result (Class::*)(Arguments...) const noexcept> : callable<Result(Arguments...)> {};

template <typename Function> struct callable : callable<decltype(&Function::operator())> {};

template <typename Function, std::size_t... Index>
consteval bool has_wiring_arguments_parameter(std::index_sequence<Index...>) {
    return (std::same_as<std::remove_cvref_t<typename callable<Function>::template argument_type<Index>>,
                         WiringArguments> || ...);
}

template <typename Function, std::size_t... Index>
consteval bool has_wiring_arguments_parameter_after_first(std::index_sequence<Index...>) {
    return (std::same_as<std::remove_cvref_t<typename callable<Function>::template argument_type<Index + 1>>,
                         WiringArguments> || ...);
}

template <typename Function, bool HasWiringArguments =
                                  has_wiring_arguments_parameter<Function>(
                                      std::make_index_sequence<callable<Function>::argument_count>{})>
struct graph_callable : callable<Function> {
    using Base = callable<Function>;
    static constexpr std::size_t argument_count = Base::argument_count;
    static constexpr bool receives_wiring_arguments = false;

    template <std::size_t Index> using argument_type = typename Base::template argument_type<Index>;
};

template <typename Function> struct graph_callable<Function, true> : callable<Function> {
    using Base = callable<Function>;
    static_assert(Base::argument_count > 0);
    static_assert(std::same_as<typename Base::template argument_type<0>, const WiringArguments &>,
                  "const WiringArguments& must be the first graph function parameter");
    static_assert(!has_wiring_arguments_parameter_after_first<Function>(
                      std::make_index_sequence<Base::argument_count - 1>{}),
                  "a graph function accepts one const WiringArguments& parameter");

    static constexpr std::size_t argument_count = Base::argument_count - 1;
    static constexpr bool receives_wiring_arguments = true;

    template <std::size_t Index> using argument_type = typename Base::template argument_type<Index + 1>;
};

template <typename Parameter> [[nodiscard]] decltype(auto) graph_argument(const WiringArgument &argument) {
    using Value = std::remove_cvref_t<Parameter>;

    if constexpr (is_typed_wiring_point<Value>) {
        static_assert(!std::is_reference_v<Parameter>);

        // Signature resolution already validated the annotation. The wiring point retains its actual representation.
        return Value::from_validated(argument.time_series());
    } else if constexpr (std::same_as<Value, WiringPoint>) {
        static_assert(!std::is_reference_v<Parameter>);

        return argument.time_series();
    } else if constexpr (std::same_as<Value, GraphArgument>) {
        static_assert(!std::is_reference_v<Parameter> ||
                      (std::is_lvalue_reference_v<Parameter> &&
                       std::is_const_v<std::remove_reference_t<Parameter>>),
                      "a graph argument parameter must be a value or const lvalue reference");

        return argument.graph();
    } else if constexpr (graph_scalar_view_argument<Value>) {
        static_assert(!std::is_reference_v<Parameter>);

        const auto scalar = argument.scalar();
        return Value::from(scalar);
    } else if constexpr (graph_argument_facade<Value>) {
        static_assert(!std::is_reference_v<Parameter> ||
                      (std::is_lvalue_reference_v<Parameter> &&
                       std::is_const_v<std::remove_reference_t<Parameter>>),
                      "a graph parameter must be a value or const lvalue reference");

        return Value{argument.graph()};
    } else if constexpr (supported_optional_scalar_argument<Value>) {
        using ApiScalar = typename optional_scalar_argument<Value>::value_type;
        using Canonical = canonical_scalar_type_t<ApiScalar>;

        if (!argument.present()) {
            return Value{};
        }
        const auto value = argument.scalar();
        return Value{scalar_api_value<ApiScalar>(scalar_cast<Canonical>(value).value())};
    } else {
        static_assert(supported_scalar_argument<Value>);
        static_assert(!std::is_lvalue_reference_v<Parameter> || std::is_const_v<std::remove_reference_t<Parameter>>);

        using Canonical = canonical_scalar_type_t<Value>;
        const auto scalar = argument.scalar();
        return scalar_api_value<Value>(scalar_cast<Canonical>(scalar).value());
    }
}

template <typename Function, std::size_t... Index>
decltype(auto) invoke_graph_function(const Function &function, std::span<const WiringArgument> arguments,
                                     std::index_sequence<Index...>) {
    using Callable = callable<Function>;

    if constexpr (sizeof...(Index) == 0) {
        static_cast<void>(arguments);
        return std::invoke(function);
    } else {
        return std::invoke(
            function,
            graph_argument<typename Callable::template argument_type<Index>>(arguments[Index])...);
    }
}

template <typename Function>
decltype(auto) invoke_graph_function(const Function &function, std::span<const WiringArgument> arguments) {
    return invoke_graph_function(function, arguments, std::make_index_sequence<callable<Function>::argument_count>{});
}

template <typename Function, std::size_t... Index>
decltype(auto) invoke_graph_function(const Function &function, const WiringArguments &wiring_arguments,
                                     std::span<const WiringArgument> arguments, std::index_sequence<Index...>) {
    return std::invoke(function, wiring_arguments,
                       graph_argument<typename graph_callable<Function>::template argument_type<Index>>(
                           arguments[Index])...);
}

template <typename Function>
decltype(auto) invoke_graph_function(const Function &function, const WiringArguments &wiring_arguments,
                                     std::span<const WiringArgument> arguments) {
    if constexpr (graph_callable<Function>::receives_wiring_arguments) {
        return invoke_graph_function(function, wiring_arguments, arguments,
                                     std::make_index_sequence<graph_callable<Function>::argument_count>{});
    } else {
        return invoke_graph_function(function, arguments);
    }
}

template <typename Function, std::size_t Index>
bool wiring_parameter_matches(const Parameter &declaration, const ResolvedParameter &parameter) {
    using Parameter = typename graph_callable<Function>::template argument_type<Index>;
    using Value = std::remove_cvref_t<Parameter>;

    if constexpr (is_typed_wiring_point<Value>) {
        if (declaration.valid_items != Value::valid_items) {
            return false;
        }
        using TimeSeries = typename typed_wiring_point<Value>::time_series_type;
        if constexpr (time_series_variable<TimeSeries>) {
            return parameter.type->is_time_series();
        } else if constexpr (type_variable_declaration<TimeSeries>) {
            return parameter.type->is_time_series() && TimeSeries::type().accepts(*parameter.type);
        } else {
            const auto &declared = [&]() -> const TimeSeriesType & {
                if constexpr (TimeSeries::has_explicit_guarantee) {
                    return time_series_type_for<Function, Index, TimeSeriesTypeRole::input, TimeSeries>(
                        guarantee_parameter<TimeSeries::guarantee>());
                } else {
                    static const auto guarantee = util::static_storage<GuaranteeVariable>{parameter.name};
                    [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(guarantee.get());
                    return time_series_type_for<Function, Index, TimeSeriesTypeRole::input, TimeSeries>(guarantee.get());
                }
            }();
            return declared.accepts(*parameter.type);
        }
    } else if constexpr (std::same_as<Value, WiringPoint>) {
        return parameter.type->is_time_series();
    } else if constexpr (std::same_as<Value, GraphArgument>) {
        return parameter.type->is_graph();
    } else if constexpr (graph_scalar_view_argument<Value>) {
        return declaration.type->accepts(*parameter.type);
    } else if constexpr (graph_argument_facade<Value>) {
        const auto *graph = type_cast<GraphArgumentType>(parameter.type);
        if (graph == nullptr || !Value::type().accepts(*graph)) {
            return false;
        }
        if constexpr (requires { Value::constraint(); }) {
            const auto &constraint = Value::constraint();
            const auto *declared = declaration.graph_constraint.get();
            const auto constrained = constraint && constraint->constrained();
            const auto declaration_constrained = declared != nullptr && declared->constrained();
            return constrained == declaration_constrained &&
                   (!constrained || constraint->same_constraint(*declared));
        }
        return declaration.graph_constraint == nullptr || !declaration.graph_constraint->constrained();
    } else if constexpr (supported_optional_scalar_argument<Value>) {
        using Scalar = typename optional_scalar_argument<Value>::value_type;

        return parameter.accepts_absent_scalar && parameter.type == &cpp_scalar_type<canonical_scalar_type_t<Scalar>>();
    } else {
        static_assert(supported_scalar_argument<Value>);

        return !parameter.accepts_absent_scalar && parameter.type == &cpp_scalar_type<canonical_scalar_type_t<Value>>();
    }
}

template <typename Function, std::size_t... Index>
[[nodiscard]] bool wiring_parameters_match(const Signature &declaration, const ResolvedSignature &signature,
                                           std::index_sequence<Index...>) {
    return (wiring_parameter_matches<Function, Index>(declaration.parameter(Index), signature.parameters[Index]) &&
            ...);
}

template <typename Function>
[[nodiscard]] bool wiring_parameters_match(const Signature &declaration, const ResolvedSignature &signature) {
    return wiring_parameters_match<Function>(declaration, signature,
                                             std::make_index_sequence<graph_callable<Function>::argument_count>{});
}

template <typename Value> util::symbol wiring_label(Value &&value) {
    if constexpr (std::same_as<std::remove_cvref_t<Value>, util::symbol>) {
        return std::forward<Value>(value);
    } else {
        return util::symbol{std::string_view{std::forward<Value>(value)}};
    }
}

struct default_wiring_label {};

} // namespace bg::details
