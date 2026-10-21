#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <concepts>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename T>
inline constexpr VariadicMode graph_parameter_variadic_mode = [] {
    if constexpr (is_typed_wiring_point<T>) {
        return T::variadic_mode;
    }
    return VariadicMode::none;
}();

template <typename T>
concept time_series_declaration = supported_time_series<T>;

template <typename Declaration> struct graph_time_series_union : std::false_type {};

template <util::compile_time_string Name, typename... Alternatives>
struct graph_time_series_union<TypeVariable<Name, Alternatives...>>
    : std::bool_constant<(supported_time_series<Alternatives> && ...)> {};

template <typename Function, std::size_t Index> const Type &graph_parameter_type(util::symbol name) {
    using Parameter = std::remove_cvref_t<typename graph_callable<Function>::template argument_type<Index>>;

    if constexpr (is_typed_wiring_point<Parameter>) {
        using TimeSeries = typename typed_wiring_point<Parameter>::time_series_type;
        static_assert(time_series_declaration<TimeSeries> || graph_time_series_union<TimeSeries>::value,
                      "wp requires a supported time series type or a union of supported time series types");

        if constexpr (time_series_variable<TimeSeries>) {
            return time_series_variable_instance<TimeSeries>();
        } else if constexpr (type_variable_declaration<TimeSeries>) {
            return TimeSeries::type();
        } else if constexpr (TimeSeries::has_explicit_guarantee) {
            return time_series_type_for<Function, Index, TimeSeriesTypeRole::input, TimeSeries>(
                guarantee_parameter<TimeSeries::guarantee>());
        } else {
            static const auto guarantee = util::static_storage<GuaranteeVariable>{name};
            [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(guarantee.get());
            return time_series_type_for<Function, Index, TimeSeriesTypeRole::input, TimeSeries>(guarantee.get());
        }
    } else if constexpr (supported_optional_scalar_argument<Parameter>) {
        using Scalar = typename optional_scalar_argument<Parameter>::value_type;

        return cpp_scalar_type<canonical_scalar_type_t<Scalar>>();
    } else if constexpr (graph_scalar_view_argument<Parameter>) {
        using Scalar = typename graph_scalar_view<Parameter>::scalar_type;
        if constexpr (scalar_variable<Scalar>) {
            return scalar_variable_instance<Scalar>();
        } else {
            return scalar_type_for<canonical_scalar_type_t<Scalar>>();
        }
    } else if constexpr (std::same_as<Parameter, GraphArgument>) {
        return GraphArgument::type();
    } else if constexpr (graph_argument_facade<Parameter>) {
        return Parameter::type();
    } else {
        static_assert(supported_scalar_argument<Parameter>,
                      "a graph function parameter must be wp, a graph argument, a scalar, or an optional scalar");

        return cpp_scalar_type<canonical_scalar_type_t<Parameter>>();
    }
}

template <typename Function, std::size_t Index>
void add_graph_parameter(std::vector<Parameter> &parameters, std::span<const std::string_view> parameter_names) {
    using Value = std::remove_cvref_t<typename graph_callable<Function>::template argument_type<Index>>;

    if constexpr (is_typed_wiring_point<Value>) {
        if constexpr (Value::context && Value::name.empty()) {
            if (parameter_names.empty()) {
                throw std::invalid_argument{"a callable graph context parameter requires a compile-time name"};
            }
        }
    }

    const auto parameter_name = [&] {
        if constexpr (is_typed_wiring_point<Value>) {
            if (!Value::name.empty()) {
                return std::string{Value::name};
            }
        }
        return parameter_names.empty() ? std::to_string(Index) : std::string{parameter_names[Index]};
    }();
    const util::symbol name{parameter_name};
    if constexpr (is_typed_wiring_point<Value>) {
        using TimeSeries = typename typed_wiring_point<Value>::time_series_type;
        constexpr auto mode = graph_parameter_variadic_mode<Value>;
        if constexpr (mode == VariadicMode::none) {
            parameters.push_back({name, &graph_parameter_type<Function, Index>(name), active, Value::validity,
                                  supported_optional_scalar_argument<Value>, mode, nullptr, Value::context, false,
                                  nullptr, Value::valid_items});
        } else {
            parameters.push_back({name, &graph_parameter_type<Function, Index>(name), active, Value::validity,
                                  supported_optional_scalar_argument<Value>, mode,
                                  variadic_wiring_operations<TimeSeries>(), false, false, nullptr,
                                  Value::valid_items});
        }
    } else {
        auto parameter = Parameter{name, &graph_parameter_type<Function, Index>(name), active, required,
                                   supported_optional_scalar_argument<Value>, graph_parameter_variadic_mode<Value>};
        if constexpr (requires { Value::constraint(); }) {
            parameter.graph_constraint = Value::constraint();
        }
        parameters.push_back(std::move(parameter));
    }
}

template <typename Function, std::size_t... Index>
[[nodiscard]] std::vector<Parameter> make_graph_parameters(
    std::span<const std::string_view> parameter_names, std::index_sequence<Index...>) {
    if (!parameter_names.empty() && parameter_names.size() != sizeof...(Index)) {
        throw std::invalid_argument{"a graph definition has the wrong number of parameter names"};
    }

    auto parameters = std::vector<Parameter>{};
    parameters.reserve(sizeof...(Index));
    (add_graph_parameter<Function, Index>(parameters, parameter_names), ...);
    return parameters;
}

template <typename Function> const TimeSeriesType &graph_output_type() {
    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    static_assert(is_typed_wiring_point<Result>, "a graph function must return void or wp");
    using TimeSeries = typename typed_wiring_point<Result>::time_series_type;
    static_assert(time_series_declaration<TimeSeries>, "wp requires a supported time series type");

    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else {
        return time_series_type_for<Function, graph_callable<Function>::argument_count, TimeSeriesTypeRole::output,
                                    TimeSeries>(
            TimeSeriesGuarantee::ordinary);
    }
}

template <typename Function, std::size_t... Index>
[[nodiscard]] Signature make_graph_signature(util::symbol name, std::span<const std::string_view> parameter_names,
                                             std::vector<VariableResolver> resolvers, std::index_sequence<Index...>) {
    auto parameters = make_graph_parameters<Function>(parameter_names, std::index_sequence<Index...>{});

    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    if constexpr (std::same_as<Result, void>) {
        return {name, std::move(parameters), std::move(resolvers)};
    } else {
        return {name, std::move(parameters), graph_output_type<Function>(), std::move(resolvers)};
    }
}

template <typename Function>
[[nodiscard]] Signature make_graph_signature(util::symbol name, std::span<const std::string_view> parameter_names,
                                             std::vector<VariableResolver> resolvers) {
    return make_graph_signature<Function>(name, parameter_names, std::move(resolvers),
                                          std::make_index_sequence<graph_callable<Function>::argument_count>{});
}

template <typename Function>
[[nodiscard]] Signature make_graph_signature(util::symbol name,
                                             std::span<const std::string_view> parameter_names = {}) {
    return make_graph_signature<Function>(name, parameter_names, {});
}

template <typename Function, std::size_t... Index>
[[nodiscard]] Signature make_graph_input_signature(util::symbol name,
                                                   std::span<const std::string_view> parameter_names,
                                                   std::index_sequence<Index...>) {
    auto parameters = make_graph_parameters<Function>(parameter_names, std::index_sequence<Index...>{});
    return {name, std::move(parameters)};
}

template <typename Function>
[[nodiscard]] Signature make_graph_input_signature(util::symbol name,
                                                   std::span<const std::string_view> parameter_names = {}) {
    return make_graph_input_signature<Function>(
        name, parameter_names, std::make_index_sequence<graph_callable<Function>::argument_count>{});
}

} // namespace bg::details
