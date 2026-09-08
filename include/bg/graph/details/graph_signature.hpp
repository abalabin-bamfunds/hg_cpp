#pragma once

#include <bg/graph/details/wiring_callable.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_variable.hpp>

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
concept time_series_declaration = supported_time_series<T>;

template <typename Function, std::size_t Index> const Type &graph_parameter_type(util::symbol name) {
    using Parameter = std::remove_cvref_t<typename callable<Function>::template argument_type<Index>>;

    if constexpr (is_typed_wiring_point<Parameter>) {
        using TimeSeries = typename typed_wiring_point<Parameter>::time_series_type;
        static_assert(time_series_declaration<TimeSeries>, "wp requires a supported time series type");

        if constexpr (time_series_variable<TimeSeries>) {
            return time_series_variable_instance<TimeSeries>();
        } else if constexpr (TimeSeries::has_explicit_guarantee) {
            return time_series_type_for<Function, Index, true, TimeSeries>(
                guarantee_parameter<TimeSeries::guarantee>());
        } else {
            static const GuaranteeVariable guarantee{name};
            return time_series_type_for<Function, Index, true, TimeSeries>(guarantee);
        }
    } else if constexpr (supported_optional_scalar_argument<Parameter>) {
        using Scalar = typename optional_scalar_argument<Parameter>::value_type;

        return cpp_scalar_type<Scalar>();
    } else {
        static_assert(supported_scalar_argument<Parameter>,
                      "a graph function parameter must be wp, a scalar, or an optional scalar");

        return cpp_scalar_type<Parameter>();
    }
}

template <typename Function, std::size_t Index>
void add_graph_parameter(std::vector<Parameter> &parameters, std::span<const std::string_view> parameter_names) {
    using Value = std::remove_cvref_t<typename callable<Function>::template argument_type<Index>>;

    const util::symbol name{parameter_names.empty() ? std::to_string(Index) : std::string{parameter_names[Index]}};
    parameters.push_back({name, &graph_parameter_type<Function, Index>(name), active, required,
                          supported_optional_scalar_argument<Value>});
}

template <typename Function> const TimeSeriesType &graph_output_type() {
    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    static_assert(is_typed_wiring_point<Result>, "a graph function must return void or wp");
    using TimeSeries = typename typed_wiring_point<Result>::time_series_type;
    static_assert(time_series_declaration<TimeSeries>, "wp requires a supported time series type");

    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else {
        return time_series_type_for<Function, callable<Function>::argument_count, false, TimeSeries>(
            TimeSeriesGuarantee::ordinary);
    }
}

template <typename Function, std::size_t... Index>
[[nodiscard]] Signature make_graph_signature(util::symbol name, std::span<const std::string_view> parameter_names,
                                             std::index_sequence<Index...>) {
    if (!parameter_names.empty() && parameter_names.size() != sizeof...(Index)) {
        throw std::invalid_argument{"a graph definition has the wrong number of parameter names"};
    }

    std::vector<Parameter> parameters;
    parameters.reserve(sizeof...(Index));
    (add_graph_parameter<Function, Index>(parameters, parameter_names), ...);

    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    if constexpr (std::same_as<Result, void>) {
        return {name, std::move(parameters)};
    } else {
        return {name, std::move(parameters), graph_output_type<Function>()};
    }
}

template <typename Function>
[[nodiscard]] Signature make_graph_signature(util::symbol name,
                                             std::span<const std::string_view> parameter_names = {}) {
    return make_graph_signature<Function>(name, parameter_names,
                                          std::make_index_sequence<callable<Function>::argument_count>{});
}

} // namespace bg::details
