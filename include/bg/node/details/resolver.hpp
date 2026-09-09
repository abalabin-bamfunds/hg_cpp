#pragma once

#include <bg/graph/wiring_arguments.hpp>
#include <bg/time_series/guarantee_variable.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/size_variable.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/guarantee_variable_instance.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/schema_variable_instance.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/size_variable_instance.hpp>
#include <bg/type/time_series_variable.hpp>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Result> TypeMapValue resolver_mapping(Result &&result, const Variable &variable) {
    using Value = std::remove_cvref_t<Result>;

    if constexpr (std::is_pointer_v<Value> && std::derived_from<std::remove_cv_t<std::remove_pointer_t<Value>>, Type>) {
        if (result == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, variable.name(),
                                      "variable resolver returned no type"};
        }

        return static_cast<const Type *>(result);
    } else if constexpr (std::derived_from<std::remove_cv_t<Value>, Type>) {
        static_assert(std::is_lvalue_reference_v<Result &&>,
                      "a resolver must return a persistent type by reference or pointer");
        return static_cast<const Type *>(std::addressof(result));
    } else if constexpr (std::same_as<Value, TimeSeriesGuarantee> || std::same_as<Value, Size>) {
        return std::forward<Result>(result);
    } else if constexpr (std::same_as<Value, std::size_t>) {
        return Size{result};
    } else {
        static_assert(std::same_as<Value, void>, "a resolver must return a type, guarantee, or size");
    }
}

template <typename Declaration> const Variable &resolver_variable() {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (scalar_variable<Value>) {
        return scalar_variable_instance<Value>();
    } else if constexpr (type_variable_declaration<Value>) {
        return Value::type();
    } else if constexpr (time_series_variable<Value> && requires { Value::name; }) {
        return TimeSeriesVariableInstance<Value>::get();
    } else if constexpr (guarantee_variable_declaration<Value>) {
        return guarantee_variable_instance<Value>();
    } else if constexpr (size_variable_declaration<Value>) {
        return size_variable_instance<Value>();
    } else if constexpr (schema_type_declaration<Value>) {
        using Traits = schema_type_traits<Value>;
        return schema_variable_instance<Traits::template declaration>();
    } else {
        static_assert(std::same_as<Value, void>, "resolve<T> requires a type, guarantee, size, or schema variable");
    }
}

template <typename Declaration, typename Function> VariableResolver variable_resolver(Function function) {
    const auto *variable = &resolver_variable<Declaration>();

    if constexpr (std::invocable<Function, const TypeMap &, const WiringArguments &>) {
        return VariableResolver{variable, [function = std::move(function), variable](const TypeMap &type_map,
                                                                                     const WiringArguments &arguments) {
                                    return resolver_mapping(std::invoke(function, type_map, arguments), *variable);
                                }};
    } else if constexpr (std::invocable<Function, const WiringArguments &>) {
        return VariableResolver{
            variable, [function = std::move(function), variable](const TypeMap &, const WiringArguments &arguments) {
                return resolver_mapping(std::invoke(function, arguments), *variable);
            }};
    } else if constexpr (std::invocable<Function, const TypeMap &>) {
        return VariableResolver{variable, [function = std::move(function), variable](const TypeMap &type_map) {
                                    return resolver_mapping(std::invoke(function, type_map), *variable);
                                }};
    } else {
        static_assert(std::invocable<Function>,
                      "a resolver must accept wiring arguments, a type map, both, or no arguments");
        return VariableResolver{variable, [function = std::move(function), variable](const TypeMap &) {
                                    return resolver_mapping(std::invoke(function), *variable);
                                }};
    }
}

} // namespace bg::details
