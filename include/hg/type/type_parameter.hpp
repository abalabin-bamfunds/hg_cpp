#pragma once

#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/schema_variable_instance.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_variable.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Declaration>
inline constexpr bool schema_type_parameter_declaration = [] {
    using Value = std::remove_cv_t<Declaration>;
    if constexpr (!schema_type_declaration<Value>) {
        return false;
    } else {
        using Traits = schema_type_traits<Value>;
        return schema_variable_declaration<typename Traits::template declaration<schema_variable_field>>;
    }
}();

template <typename Declaration>
concept type_parameter_declaration =
    scalar_variable<std::remove_cv_t<Declaration>> ||
    type_variable_declaration<std::remove_cv_t<Declaration>> ||
    (time_series_variable<std::remove_cv_t<Declaration>> && requires { std::remove_cv_t<Declaration>::name; }) ||
    schema_type_parameter_declaration<Declaration>;

template <type_parameter_declaration Declaration> decltype(auto) type_parameter() {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (scalar_variable<Value>) {
        return scalar_variable_instance<Value>();
    } else if constexpr (type_variable_declaration<Value>) {
        return Value::type();
    } else if constexpr (time_series_variable<Value>) {
        return TimeSeriesVariableInstance<Value>::get();
    } else {
        using Traits = schema_type_traits<Value>;
        return schema_variable_instance<Traits::template declaration>();
    }
}

template <typename... Declaration> struct type_parameterized_declaration {
    static_assert(sizeof...(Declaration) != 0);

    static constexpr auto count = sizeof...(Declaration);
    using Types = std::tuple<Declaration...>;
    using Definition = std::tuple_element_t<count - 1, Types>;

    template <std::size_t... Index> static consteval bool valid_parameters(std::index_sequence<Index...>) {
        return (type_parameter_declaration<std::tuple_element_t<Index, Types>> && ...);
    }

    static constexpr auto has_valid_parameters = valid_parameters(std::make_index_sequence<count - 1>{});
};

} // namespace bg::details
