#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/node/details/aggregate_node.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/compile_time_string.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Scope> struct CallableOutputGuarantee {
    static constexpr util::compile_time_string name{"callable_output"};
};

template <typename Scope>
struct is_guarantee_variable_declaration<CallableOutputGuarantee<Scope>> : std::true_type {};

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
             promoted_field<callable_field<Function, Index>>::value ||
             scalar_field<callable_field<Function, Index>>::value) &&
            ...);
}

template <typename Result> struct callable_return_output {
    static constexpr bool value = false;
    static constexpr bool optional = false;
    using type = void;
};

template <typename Result>
    requires(scalar_field<std::remove_cvref_t<Result>>::value &&
             !scalar_view<std::remove_cvref_t<Result>>::value)
struct callable_return_output<Result> {
    static constexpr bool value = true;
    static constexpr bool optional = scalar_field<std::remove_cvref_t<Result>>::optional;
    using Value = typename scalar_field<std::remove_cvref_t<Result>>::parameter_type;
    using type = Out<TS<Value>>;
};

template <typename Field>
inline constexpr bool callable_output_guarantee_field = [] {
    if constexpr (input_field<Field>::value) {
        return input_field<Field>::validity == required && !input_field<Field>::dynamic_activity;
    } else if constexpr (promoted_field<Field>::value || scalar_field<Field>::value) {
        return !scalar_field<Field>::optional;
    } else {
        return false;
    }
}();

template <typename Function, std::size_t... Index>
consteval bool callable_output_guarantee_fields(std::index_sequence<Index...>) {
    return (callable_output_guarantee_field<callable_field<Function, Index>> && ...);
}

template <typename Function>
inline constexpr bool callable_output_guarantee_candidate =
    callable_return_output<typename callable<Function>::result_type>::value &&
    !callable_return_output<typename callable<Function>::result_type>::optional &&
    callable_output_guarantee_fields<Function>(
        std::make_index_sequence<callable<Function>::argument_count>{});

template <typename Function, bool = callable_output_guarantee_candidate<Function>> struct callable_result_declaration {
    using type = typename callable_return_output<typename callable<Function>::result_type>::type;
};

template <typename Function> struct callable_result_declaration<Function, true> {
    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    using Value = typename scalar_field<Result>::parameter_type;
    using type = Out<TS<Value, CallableOutputGuarantee<Function>{}>>;
};

template <typename Function>
inline constexpr bool callable_result_supported =
    std::same_as<typename callable<Function>::result_type, void> ||
    callable_return_output<typename callable<Function>::result_type>::value;

template <typename Function, std::size_t... Index>
auto callable_output_declaration(std::index_sequence<Index...>) -> typename first_output<
    typename output_field<callable_field<Function, Index>>::output_type...,
    typename callable_result_declaration<Function>::type>::type;

template <typename Function>
using callable_output_declaration_t =
    decltype(callable_output_declaration<Function>(std::make_index_sequence<callable<Function>::argument_count>{}));

template <typename Function>
using callable_output_t = typename output_field<callable_output_declaration_t<Function>>::time_series_type;

template <typename Function>
using callable_wiring_output_t = std::conditional_t<
    callable_output_guarantee_candidate<Function>,
    typename output_field<
        typename callable_return_output<typename callable<Function>::result_type>::type>::time_series_type,
    callable_output_t<Function>>;

template <typename Scope, typename Function, std::size_t Index>
const TimeSeriesType &callable_input_type(util::symbol name) {
    using Field = callable_field<Function, Index>;
    using TimeSeries = typename input_field<Field>::time_series_type;

    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else if constexpr (TimeSeries::has_explicit_guarantee) {
        return time_series_type_for<Scope, Index, TimeSeriesTypeRole::input, TimeSeries>(
            guarantee_parameter<TimeSeries::guarantee>());
    } else {
        static const auto guarantee = GuaranteeVariable{name};
        [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(guarantee);
        return time_series_type_for<Scope, Index, TimeSeriesTypeRole::input, TimeSeries>(guarantee);
    }
}

template <typename Scope, typename TimeSeries, std::size_t Index> const TimeSeriesType &callable_output_type() {
    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else {
        return time_series_type_for<Scope, Index, TimeSeriesTypeRole::output, TimeSeries>(
            TimeSeriesGuarantee::ordinary);
    }
}

template <typename Function>
VariableResolver callable_output_guarantee_resolver(std::vector<Parameter> parameters,
                                                    std::optional<bool> schedule_on_start) {
    using Guarantee = CallableOutputGuarantee<Function>;
    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    using Value = typename scalar_field<Result>::parameter_type;
    const auto &variable = guarantee_variable_instance<Guarantee>();
    const auto *output_value_type = &scalar_type_for<Value>();

    return VariableResolver{
        &variable,
        [parameters = std::move(parameters), schedule_on_start, output_value_type](const TypeMap &type_map) {
            auto guarantee = TimeSeriesGuarantee::const_;
            auto has_active_time_series = false;
            auto all_scalar = true;

            for (const auto &parameter : parameters) {
                const auto &type = parameter.type->resolve(type_map);
                if (type.is_scalar()) {
                    continue;
                }

                const auto &time_series = static_cast<const TimeSeriesType &>(type);
                guarantee = weakest(guarantee, time_series.guarantee().resolve(type_map));
                all_scalar = false;
                has_active_time_series = has_active_time_series || parameter.activity == active;
            }

            const auto scheduled_on_start = schedule_on_start.value_or(all_scalar);
            if (scheduled_on_start && !all_scalar) {
                return TimeSeriesGuarantee::ordinary;
            }

            if (guarantee == TimeSeriesGuarantee::const_ && !scheduled_on_start && !has_active_time_series) {
                return TimeSeriesGuarantee::ordinary;
            }

            const auto &resolved_output_value_type =
                static_cast<const ScalarType &>(output_value_type->resolve(type_map));
            if (guarantee == TimeSeriesGuarantee::deduplicated && !resolved_output_value_type.supports_equality()) {
                return TimeSeriesGuarantee::ordinary;
            }

            return guarantee;
        }};
}

template <typename Scope, typename Function, std::size_t Index>
void add_callable_parameter(std::vector<Parameter> &parameters, std::vector<VariableResolver> &resolvers) {
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
    } else if constexpr (promoted_field<Field>::value) {
        const auto name = util::symbol{std::to_string(parameters.size())};
        using Value = typename promoted_field<Field>::value_type;
        using Parameter = typename promoted_field<Field>::parameter_type;
        using TimeSeries = TS<Parameter>;
        constexpr auto validity = promoted_field<Field>::optional ? InputValidity::optional : InputValidity::required;
        parameters.push_back(
            {name, &promoted_type<Scope, Index, Parameter>(name), active, validity, promoted_field<Field>::optional});
        append_aggregate_resolvers<Scope, Value>(resolvers);
        append_aggregate_resolvers<Scope, TimeSeries>(resolvers);
    } else {
        static_assert(scalar_field<Field>::value,
                      "a callable parameter must be In, a scalar, an optional scalar, or Out");
        using Value = typename scalar_field<Field>::parameter_type;
        parameters.push_back({util::symbol{std::to_string(parameters.size())}, &scalar_type_for<Value>(), active,
                              required, scalar_field<Field>::optional});
    }
}

template <typename Scope, typename Function, std::size_t Index>
void add_callable_field(std::vector<Parameter> &parameters, std::vector<VariableResolver> &resolvers,
                        const TimeSeriesType *&output) {
    using Field = callable_field<Function, Index>;

    if constexpr (output_field<Field>::value) {
        using TimeSeries = typename output_field<Field>::time_series_type;
        output = &callable_output_type<Scope, TimeSeries, Index>();
    } else {
        add_callable_parameter<Scope, Function, Index>(parameters, resolvers);
    }
}

template <typename Scope, typename Function, std::size_t... Index>
Signature make_callable_signature(util::symbol name, std::index_sequence<Index...>,
                                  std::vector<VariableResolver> resolvers = {},
                                  std::optional<bool> schedule_on_start = std::nullopt) {
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(sizeof...(Index));
    const TimeSeriesType *output = nullptr;
    (add_callable_field<Scope, Function, Index>(parameters, resolvers, output), ...);

    using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
    if constexpr (callable_return_output<Result>::value) {
        if (output == nullptr) {
            using TimeSeries = callable_output_t<Function>;
            output = &callable_output_type<Scope, TimeSeries, sizeof...(Index)>();
            if constexpr (callable_output_guarantee_candidate<Function>) {
                resolvers.push_back(callable_output_guarantee_resolver<Function>(parameters, schedule_on_start));
            }
        }
    }

    return output == nullptr ? Signature{name, std::move(parameters), std::move(resolvers)}
                             : Signature{name, std::move(parameters), *output, std::move(resolvers)};
}

template <typename Scope, typename Function>
Signature make_callable_signature(util::symbol name, std::vector<VariableResolver> resolvers = {},
                                  std::optional<bool> schedule_on_start = std::nullopt) {
    return make_callable_signature<Scope, Function>(
        name, std::make_index_sequence<callable<Function>::argument_count>{}, std::move(resolvers),
        schedule_on_start);
}

} // namespace bg::details
