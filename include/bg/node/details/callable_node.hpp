#pragma once

#include <bg/graph/requirement.hpp>
#include <bg/node/details/callable_node_options.hpp>
#include <bg/node/details/callable_signature.hpp>
#include <bg/node/details/resolver.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/util/reflection.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Function>
concept callable_node =
    requires { &Function::operator(); } && std::copy_constructible<Function> &&
    std::same_as<typename callable<Function>::result_type, void> &&
    callable_fields_supported<Function>(std::make_index_sequence<callable<Function>::argument_count>{}) &&
    callable_output_count<Function>(std::make_index_sequence<callable<Function>::argument_count>{}) <= 1;

template <typename Option>
void append_callable_resolver(std::vector<VariableResolver> &resolvers, const Option &option) {
    if constexpr (is_callable_resolver_v<Option>) {
        resolvers.push_back(variable_resolver<typename Option::declaration_type>(option.function));
    }
}

template <typename... Options> std::vector<VariableResolver> callable_resolvers(const std::tuple<Options...> &options) {
    auto result = std::vector<VariableResolver>{};
    result.reserve((std::size_t{} + ... + static_cast<std::size_t>(is_callable_resolver_v<Options>)));

    std::apply([&](const auto &...option) { (append_callable_resolver(result, option), ...); }, options);

    return result;
}

template <typename Function, std::size_t Target, std::size_t... Index>
consteval std::size_t callable_parameter_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(Index < Target && !output_field<callable_field<Function, Index>>::value));
}

template <typename Function, std::size_t Target, std::size_t... Index>
consteval std::size_t callable_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(Index < Target && input_field<callable_field<Function, Index>>::value));
}

template <typename Function, std::size_t Target, std::size_t... Index>
consteval std::size_t callable_scalar_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(Index < Target && scalar_field<callable_field<Function, Index>>::value));
}

template <typename Function, std::size_t Index, typename Invocation>
decltype(auto) make_callable_node_argument(Invocation invocation) {
    using Field = callable_field<Function, Index>;
    constexpr auto Indices = std::make_index_sequence<callable<Function>::argument_count>{};

    if constexpr (input_field<Field>::value) {
        constexpr auto Parameter = callable_parameter_index<Function, Index>(Indices);
        constexpr auto Input = callable_input_index<Function, Index>(Indices);
        const auto value = node_invocation_access::input(invocation, Parameter, Input).value();
        return Field{value.last_updated(), value.data(), value.runtime_type()};
    } else if constexpr (output_field<Field>::value) {
        using TimeSeries = typename output_field<Field>::time_series_type;
        return node_invocation_access::output<TimeSeries>(invocation);
    } else {
        using Value = typename scalar_field<Field>::value_type;
        constexpr auto Parameter = callable_parameter_index<Function, Index>(Indices);
        constexpr auto ScalarIndex = callable_scalar_index<Function, Index>(Indices);
        const auto scalar = node_invocation_access::scalar(invocation, Parameter, ScalarIndex);

        if constexpr (scalar_field<Field>::optional) {
            return scalar.has_value() ? Field{scalar_cast<Value>(scalar.value()).value()} : Field{};
        } else if constexpr (scalar_view<Field>::value) {
            if constexpr (generic_scalar<Value>) {
                return Field::from(scalar.value().input_view());
            } else {
                return scalar_cast<Value>(scalar.value());
            }
        } else {
            return scalar_cast<Value>(scalar.value()).value();
        }
    }
}

template <typename Function, typename... Options> class CallableNodeDefinition {
    static constexpr auto stores_function = !std::is_empty_v<Function> || !std::default_initializable<Function>;

  public:
    using compiled_data_type = std::conditional_t<stores_function, Function, void>;
    using output_time_series_type = callable_output_t<Function>;
    using Invocation = NodeInvocation<compiled_data_type>;

    explicit CallableNodeDefinition(Function function, Options... options)
        : function_{std::move(function)}, options_{std::move(options)...},
          signature_{make_callable_signature<Function, Function>(util::symbol{util::unqualified_type_name<Function>()},
                                                                 callable_resolvers(options_))} {}

    const Signature &signature() const { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const {
        auto result = accept;

        std::apply(
            [&](const auto &...option) {
                (
                    [&] {
                        if constexpr (is_callable_requirement_v<decltype(option)>) {
                            result = requirement_result(std::invoke(option.function, arguments));
                        }
                    }(),
                    ...);
            },
            options_);

        return result;
    }

    Function compile(const ResolvedSignature &, std::pmr::memory_resource &) const
        requires stores_function
    {
        return function_;
    }

    const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &signature,
                        std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                        std::span<const TimeSeriesRuntimeType *const> input_runtime_types) const {
        if constexpr (std::same_as<output_time_series_type, void>) {
            return nullptr;
        } else if constexpr (generic_time_series<output_time_series_type>) {
            return &resolve_generic_output_runtime_type(signature, parameter_runtime_types, input_runtime_types);
        } else {
            return &cpp_time_series_runtime_type<output_time_series_type>(*signature.output);
        }
    }

    static void evaluate(Invocation invocation) {
        invoke(function(invocation), invocation, std::make_index_sequence<callable<Function>::argument_count>{});
    }

    static void on_invalidation(Invocation invocation)
        requires(!std::same_as<output_time_series_type, void> &&
                 (time_series_variable<output_time_series_type> || is_time_series_reference<output_time_series_type> ||
                  is_time_series_list<output_time_series_type>))
    {
        node_invocation_access::output<output_time_series_type>(invocation).invalidate();
    }

  private:
    template <std::size_t... Index>
    static void invoke(const Function &function, Invocation invocation, std::index_sequence<Index...>) {
        std::invoke(function, make_callable_node_argument<Function, Index>(invocation)...);
    }

    static const Function &function(Invocation invocation) {
        if constexpr (stores_function) {
            return invocation.compiled_data();
        } else {
            static const auto value = Function{};
            return value;
        }
    }

    [[no_unique_address]] Function function_;
    [[no_unique_address]] std::tuple<Options...> options_;
    Signature signature_;
};

} // namespace bg::details
