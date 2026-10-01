#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/node/details/aggregate_node.hpp>
#include <bg/type/details/time_series_type.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Function, std::size_t Index>
using callable_field = std::remove_cvref_t<typename callable<Function>::template argument_type<Index>>;

template <typename Function, std::size_t... Index>
consteval std::size_t callable_output_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + static_cast<std::size_t>(output_field<callable_field<Function, Index>>::value));
}

template <typename Function, std::size_t... Index>
consteval bool callable_fields_supported(std::index_sequence<Index...>) {
    return ((input_field<callable_field<Function, Index>>::value ||
             output_field<callable_field<Function, Index>>::value ||
             scalar_field<callable_field<Function, Index>>::value) &&
            ...);
}

template <typename Function, std::size_t... Index>
auto callable_output(std::index_sequence<Index...>) -> typename first_output_time_series<
    typename output_field<callable_field<Function, Index>>::time_series_type...>::type;

template <typename Function>
using callable_output_t =
    decltype(callable_output<Function>(std::make_index_sequence<callable<Function>::argument_count>{}));

template <typename Scope, typename Function, std::size_t Index>
const TimeSeriesType &callable_input_type(util::symbol name) {
    using Field = callable_field<Function, Index>;
    using TimeSeries = typename input_field<Field>::time_series_type;

    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else if constexpr (TimeSeries::has_explicit_guarantee) {
        return time_series_type_for<Scope, Index, true, TimeSeries>(guarantee_parameter<TimeSeries::guarantee>());
    } else {
        static const auto guarantee = GuaranteeVariable{name};
        return time_series_type_for<Scope, Index, true, TimeSeries>(guarantee);
    }
}

template <typename Scope, typename TimeSeries, std::size_t Index> const TimeSeriesType &callable_output_type() {
    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else {
        return time_series_type_for<Scope, Index, false, TimeSeries>(TimeSeriesGuarantee::ordinary);
    }
}

template <typename Scope, typename Function, std::size_t Index>
void add_callable_parameter(std::vector<Parameter> &parameters) {
    using Field = callable_field<Function, Index>;

    if constexpr (input_field<Field>::value) {
        using TimeSeries = typename input_field<Field>::time_series_type;
        static_assert(!input_field<Field>::dynamic_binding,
                      "dynamic_binding is supported only by aggregate node fields");
        const auto text = Field::name.empty() ? std::to_string(parameters.size()) : std::string{Field::name};
        const auto name = util::symbol{text};
        static_assert(!input_field<Field>::context || !Field::name.empty(),
                      "a callable context input requires a compile-time name");
        constexpr auto mode = input_field<Field>::variadic_mode;
        if constexpr (mode == VariadicMode::none) {
            parameters.push_back({name, &callable_input_type<Scope, Function, Index>(name), Field::activity,
                                  Field::validity, false, mode, nullptr, input_field<Field>::context,
                                  input_field<Field>::dynamic_activity});
        } else {
            parameters.push_back({name, &callable_input_type<Scope, Function, Index>(name), Field::activity,
                                  Field::validity, false, mode, variadic_wiring_operations<TimeSeries>(), false,
                                  input_field<Field>::dynamic_activity});
        }
    } else {
        static_assert(scalar_field<Field>::value,
                      "a callable parameter must be In, a scalar, an optional scalar, or Out");
        using Value = typename scalar_field<Field>::parameter_type;
        parameters.push_back({util::symbol{std::to_string(parameters.size())}, &scalar_type_for<Value>(), active,
                              required, scalar_field<Field>::optional});
    }
}

template <typename Scope, typename Function, std::size_t Index>
void add_callable_field(std::vector<Parameter> &parameters, const TimeSeriesType *&output) {
    using Field = callable_field<Function, Index>;

    if constexpr (output_field<Field>::value) {
        using TimeSeries = typename output_field<Field>::time_series_type;
        output = &callable_output_type<Scope, TimeSeries, Index>();
    } else {
        add_callable_parameter<Scope, Function, Index>(parameters);
    }
}

template <typename Scope, typename Function, std::size_t... Index>
Signature make_callable_signature(util::symbol name, std::index_sequence<Index...>,
                                  std::vector<VariableResolver> resolvers = {}) {
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(sizeof...(Index));
    const TimeSeriesType *output = nullptr;
    (add_callable_field<Scope, Function, Index>(parameters, output), ...);

    return output == nullptr ? Signature{name, std::move(parameters), std::move(resolvers)}
                             : Signature{name, std::move(parameters), *output, std::move(resolvers)};
}

template <typename Scope, typename Function>
Signature make_callable_signature(util::symbol name, std::vector<VariableResolver> resolvers = {}) {
    return make_callable_signature<Scope, Function>(
        name, std::make_index_sequence<callable<Function>::argument_count>{}, std::move(resolvers));
}

} // namespace bg::details
