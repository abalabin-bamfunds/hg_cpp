#pragma once

#include <bg/graph/requirement.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/node/details/callable_node_options.hpp>
#include <bg/node/details/callable_signature.hpp>
#include <bg/node/details/resolver.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/util/reflection.hpp>

#include <concepts>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Function>
concept callable_node =
    requires { &Function::operator(); } && std::copy_constructible<Function> &&
    callable_result_supported<Function> &&
    callable_fields_supported<Function>(std::make_index_sequence<callable<Function>::argument_count>{}) &&
    callable_output_count<Function>(std::make_index_sequence<callable<Function>::argument_count>{}) <= 1 &&
    (callable_output_count<Function>(std::make_index_sequence<callable<Function>::argument_count>{}) == 0 ||
     std::same_as<typename callable<Function>::result_type, void>);

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

template <typename Option> bool callable_wiring_deduplication(const Option &option) noexcept {
    if constexpr (is_callable_wiring_deduplication_v<Option>) {
        return option.enabled;
    }
    return true;
}

template <typename... Options>
std::optional<bool> callable_schedule_on_start_option(const std::tuple<Options...> &options) noexcept {
    auto result = std::optional<bool>{};
    std::apply(
        [&](const auto &...option) {
            (([&] {
                 if constexpr (is_callable_start_scheduling_v<decltype(option)>) {
                     result = option.enabled;
                 }
             }()),
             ...);
        },
        options);
    return result;
}

template <typename... Options>
inline constexpr bool callable_has_output_runtime_type_option =
    (false || ... || is_callable_output_runtime_type_v<Options>);

template <typename Function, std::size_t Target, std::size_t... Index>
consteval std::size_t callable_parameter_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(Index < Target && !output_field<callable_field<Function, Index>>::value));
}

template <typename Function, std::size_t Target, std::size_t... Index>
consteval std::size_t callable_dynamic_activity_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(Index < Target && input_field<callable_field<Function, Index>>::dynamic_activity));
}

template <typename Function, std::size_t Index, typename Invocation>
auto make_callable_node_argument(Invocation invocation) {
    using Field = callable_field<Function, Index>;
    constexpr auto Indices = std::make_index_sequence<callable<Function>::argument_count>{};

    if constexpr (input_field<Field>::value) {
        constexpr auto Parameter = callable_parameter_index<Function, Index>(Indices);
        constexpr auto ControlledInput = callable_dynamic_activity_input_index<Function, Index>(Indices);
        return invocation.template input<Field, ControlledInput>(Parameter);
    } else if constexpr (promoted_field<Field>::value) {
        using Value = typename promoted_field<Field>::value_type;
        using ParameterScalar = typename promoted_field<Field>::parameter_type;
        constexpr auto Parameter = callable_parameter_index<Function, Index>(Indices);

        if (invocation.parameter_is_time_series(Parameter)) {
            using Input = In<TS<ParameterScalar>>;
            const auto input = invocation.template input<Input>(Parameter);
            if constexpr (promoted_field<Field>::optional) {
                if (!input.valid()) {
                    return Field{};
                }
            }
            const auto value = scalar_api_value<Value>(input.value());
            if constexpr (promoted_field<Field>::optional) {
                return Field{std::move(value)};
            } else {
                return value;
            }
        }

        const auto scalar = invocation.scalar(Parameter);
        if constexpr (promoted_field<Field>::optional) {
            if (!scalar.has_value()) {
                return Field{};
            }
        }
        auto value = [&] {
            if constexpr (scalar_input_requires_runtime_type<ParameterScalar>()) {
                return scalar_api_value<Value>(scalar_cast<ParameterScalar>(scalar.value()).value());
            } else {
                return scalar_api_value<Value>(scalar.template value<ParameterScalar>());
            }
        }();
        if constexpr (promoted_field<Field>::optional) {
            return Field{std::move(value)};
        } else {
            return value;
        }
    } else if constexpr (output_field<Field>::value) {
        return invocation.template output_as<Field>();
    } else {
        using Value = typename scalar_field<Field>::value_type;
        using ParameterScalar = typename scalar_field<Field>::parameter_type;
        constexpr auto ParameterIndex = callable_parameter_index<Function, Index>(Indices);
        const auto scalar = invocation.scalar(ParameterIndex);

        if constexpr (scalar_field<Field>::optional) {
            if (!scalar.has_value()) {
                return Field{};
            }
            if constexpr (scalar_input_requires_runtime_type<ParameterScalar>()) {
                return Field{scalar_api_value<Value>(scalar_cast<ParameterScalar>(scalar.value()).value())};
            } else {
                return Field{scalar_api_value<Value>(scalar.template value<ParameterScalar>())};
            }
        } else if constexpr (scalar_view<Field>::value) {
            if constexpr (scalar_input_requires_runtime_type<ParameterScalar>()) {
                const auto value = scalar.value();
                return Field::from(value.runtime_type(), value.data());
            } else {
                using Stored = scalar_stored_t<ParameterScalar>;
                return Field::from(*static_cast<const Stored *>(invocation.scalar_data(ParameterIndex)));
            }
        } else if constexpr (scalar_input_requires_runtime_type<ParameterScalar>()) {
            return scalar_api_value<Value>(scalar_cast<ParameterScalar>(scalar.value()).value());
        } else {
            return scalar_api_value<Value>(scalar.template value<ParameterScalar>());
        }
    }
}

template <typename Function, typename... Options> class CallableNodeDefinition {
    static constexpr auto stores_function = !std::is_empty_v<Function> || !std::default_initializable<Function>;

  public:
    using compiled_data_type = std::conditional_t<stores_function, Function, void>;
    using output_declaration_type = callable_output_declaration_t<Function>;
    using output_time_series_type = callable_wiring_output_t<Function>;
    using Invocation = NodeInvocation<compiled_data_type>;

    static_assert(!callable_has_output_runtime_type_option<Options...> ||
                      !std::same_as<output_declaration_type, void>,
                  "an output runtime type option requires a callable output declaration");

    explicit CallableNodeDefinition(Function function, Options... options)
        : function_{std::move(function)}, options_{std::move(options)...},
          signature_{make_callable_signature<Function, Function>(util::symbol{util::unqualified_type_name<Function>()},
                                                                 callable_resolvers(options_),
                                                                 callable_schedule_on_start_option(options_))} {}

    const Signature &signature() const { return signature_; }

    bool deduplicate_wiring() const noexcept {
        return std::apply([](const auto &...option) { return (callable_wiring_deduplication(option) && ...); },
                          options_);
    }

    bool schedule_on_start(const ResolvedSignature &signature) const noexcept {
        if (const auto option = callable_schedule_on_start_option(options_); option.has_value()) {
            return *option;
        }

        return std::ranges::all_of(signature.parameters,
                                   [](const auto &parameter) { return parameter.type->is_scalar(); });
    }

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

    Function compile(const ResolvedSignature &, allocator &) const
        requires stores_function
    {
        return function_;
    }

    static void evaluate(Invocation invocation) {
        using Result = std::remove_cvref_t<typename callable<Function>::result_type>;
        if constexpr (std::same_as<Result, void>) {
            invoke(function(invocation), invocation, std::make_index_sequence<callable<Function>::argument_count>{});
        } else {
            auto value = invoke(function(invocation), invocation,
                                std::make_index_sequence<callable<Function>::argument_count>{});
            auto output = invocation.template output_as<output_declaration_type>();
            if constexpr (scalar_field<Result>::optional) {
                if (value.has_value()) {
                    output.set(*value);
                }
            } else {
                output.set(std::move(value));
            }
        }
    }

    static void on_invalidation(Invocation invocation)
        requires(!std::same_as<output_declaration_type, void> &&
                 requires(Invocation value) {
                     value.template output_as<output_declaration_type>().invalidate();
                 })
    {
        invocation.template output_as<output_declaration_type>().invalidate();
    }

    bool uses_node_invocation_parameters() const noexcept { return false; }

    bool uses_time_series_input_runtime_type(const ResolvedSignature &, std::size_t parameter_index) const noexcept {
        return parameter_uses_runtime_type<true>(parameter_index,
                                                 std::make_index_sequence<callable<Function>::argument_count>{});
    }

    bool uses_scalar_input_runtime_type(const ResolvedSignature &, std::size_t parameter_index) const noexcept {
        return parameter_uses_runtime_type<false>(parameter_index,
                                                  std::make_index_sequence<callable<Function>::argument_count>{});
    }

    bool uses_output_runtime_type() const noexcept {
        return !std::same_as<output_declaration_type, void> &&
               (output_requires_runtime_type<output_declaration_type> ||
                callable_has_output_runtime_type_option<Options...>);
    }

    const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &arguments) const
        requires(!std::same_as<output_declaration_type, void> &&
                 callable_has_output_runtime_type_option<Options...>)
    {
        const auto *selected = static_cast<const TimeSeriesRuntimeType *>(nullptr);
        std::apply(
            [&](const auto &...option) {
                (([&] {
                     if constexpr (is_callable_output_runtime_type_v<decltype(option)>) {
                         selected = std::addressof(std::invoke(option.function, arguments));
                     }
                 }()),
                 ...);
            },
            options_);

        return *selected;
    }

  private:
    template <std::size_t... Index>
    static decltype(auto) invoke(const Function &function, Invocation invocation, std::index_sequence<Index...>) {
        return std::invoke(function, make_callable_node_argument<Function, Index>(invocation)...);
    }

    static const Function &function(Invocation invocation) {
        if constexpr (stores_function) {
            return invocation.compiled_data();
        } else {
            static const auto value = Function{};
            return value;
        }
    }

    template <bool TimeSeries, std::size_t... Index>
    static bool parameter_uses_runtime_type(std::size_t parameter_index, std::index_sequence<Index...>) noexcept {
        auto result = false;
        (([&] {
             using Field = callable_field<Function, Index>;
             if constexpr (!output_field<Field>::value) {
                 if (callable_parameter_index<Function, Index>(
                     std::make_index_sequence<callable<Function>::argument_count>{}) == parameter_index) {
                     if constexpr (TimeSeries && input_field<Field>::value) {
                         result = input_requires_runtime_type<Field>;
                     } else if constexpr (TimeSeries && promoted_field<Field>::value) {
                         using Input = In<TS<typename promoted_field<Field>::parameter_type>>;
                         result = input_requires_runtime_type<Input>;
                     } else if constexpr (!TimeSeries && promoted_field<Field>::value) {
                         result = scalar_input_requires_runtime_type<typename promoted_field<Field>::parameter_type>();
                     } else if constexpr (!TimeSeries && scalar_field<Field>::value) {
                         result = scalar_input_requires_runtime_type<typename scalar_field<Field>::parameter_type>();
                     }
                 }
             }
         }()),
         ...);
        return result;
    }

    [[no_unique_address]] Function function_;
    [[no_unique_address]] std::tuple<Options...> options_;
    Signature signature_;
};

} // namespace bg::details
