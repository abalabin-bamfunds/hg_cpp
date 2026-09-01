#pragma once

#include <bg/graph/details/wiring_argument.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
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

template <typename Parameter> [[nodiscard]] decltype(auto) graph_argument(wiring_argument &argument) {
    using Value = std::remove_cvref_t<Parameter>;

    if constexpr (is_typed_wiring_point<Value>) {
        static_assert(!std::is_reference_v<Parameter>);

        return Value{argument.wiring_point()};
    } else if constexpr (std::same_as<Value, WiringPoint>) {
        static_assert(!std::is_reference_v<Parameter>);

        return argument.wiring_point();
    } else if constexpr (supported_optional_scalar_argument<Value>) {
        using Scalar = typename optional_scalar_argument<Value>::value_type;

        return argument.scalar_has_value() ? Value{*static_cast<const Scalar *>(argument.scalar_data())} : Value{};
    } else {
        static_assert(supported_scalar_argument<Value>);
        static_assert(!std::is_lvalue_reference_v<Parameter> || std::is_const_v<std::remove_reference_t<Parameter>>);

        const auto &value = *static_cast<const Value *>(argument.scalar_data());
        if constexpr (std::is_lvalue_reference_v<Parameter>) {
            return value;
        } else {
            return Value{value};
        }
    }
}

template <typename Function, std::size_t... Index>
decltype(auto) invoke_graph_function(const Function &function, std::span<wiring_argument> arguments,
                                     std::index_sequence<Index...>) {
    using Callable = callable<Function>;

    return std::invoke(function, graph_argument<typename Callable::template argument_type<Index>>(arguments[Index])...);
}

template <typename Function>
decltype(auto) invoke_graph_function(const Function &function, std::span<wiring_argument> arguments) {
    return invoke_graph_function(function, arguments, std::make_index_sequence<callable<Function>::argument_count>{});
}

template <typename Function, std::size_t Index>
[[nodiscard]] bool wiring_parameter_matches(const ResolvedParameter &parameter) {
    using Parameter = typename callable<Function>::template argument_type<Index>;
    using Value = std::remove_cvref_t<Parameter>;

    if constexpr (is_typed_wiring_point<Value>) {
        using TimeSeries = typename typed_wiring_point<Value>::time_series_type;
        if constexpr (time_series_variable<TimeSeries>) {
            return parameter.type->is_time_series();
        } else {
            const auto &declared = [&]() -> const TimeSeriesType & {
                if constexpr (TimeSeries::has_explicit_guarantee) {
                    return time_series_type_for<Function, Index, true, TimeSeries>(
                        guarantee_parameter<Function, TimeSeries::guarantee>());
                } else {
                    static const auto guarantee = GuaranteeVariable{parameter.name};
                    return time_series_type_for<Function, Index, true, TimeSeries>(guarantee);
                }
            }();
            auto bindings = TypeMap{};
            return declared.match(*parameter.type, bindings);
        }
    } else if constexpr (std::same_as<Value, WiringPoint>) {
        return parameter.type->is_time_series();
    } else if constexpr (supported_optional_scalar_argument<Value>) {
        using Scalar = typename optional_scalar_argument<Value>::value_type;

        return parameter.accepts_absent_scalar && parameter.type == &cpp_scalar_type<Scalar>();
    } else {
        static_assert(supported_scalar_argument<Value>);

        return !parameter.accepts_absent_scalar && parameter.type == &cpp_scalar_type<Value>();
    }
}

template <typename Function, std::size_t... Index>
[[nodiscard]] bool wiring_parameters_match(const ResolvedSignature &signature, std::index_sequence<Index...>) {
    return (wiring_parameter_matches<Function, Index>(signature.parameters[Index]) && ...);
}

template <typename Function> [[nodiscard]] bool wiring_parameters_match(const ResolvedSignature &signature) {
    return wiring_parameters_match<Function>(signature, std::make_index_sequence<callable<Function>::argument_count>{});
}

template <typename Value> [[nodiscard]] util::symbol wiring_label(Value &&value) {
    if constexpr (std::same_as<std::remove_cvref_t<Value>, util::symbol>) {
        return std::forward<Value>(value);
    } else {
        return util::symbol{std::string_view{std::forward<Value>(value)}};
    }
}

struct default_wiring_label {};

} // namespace bg::details
