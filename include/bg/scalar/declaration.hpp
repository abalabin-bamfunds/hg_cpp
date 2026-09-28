#pragma once

#include <bg/scalar/details/type_access.hpp>
#include <bg/util/compile_time_string.hpp>

#include <concepts>
#include <type_traits>

namespace bg {

struct EqualityComparable {};
struct Hashable {};

namespace details {
struct hierarchical_scalar_constraint {};
struct struct_scalar_constraint {};
}

template <util::compile_time_string Name, typename... Constraints>
struct ScalarVariable : details::scalar_type_access<ScalarVariable<Name, Constraints...>> {
    static constexpr auto name = Name;
};

using SCALAR = ScalarVariable<"SCALAR">;
using HIERARCHICAL = ScalarVariable<"HIERARCHICAL", details::hierarchical_scalar_constraint>;
using STRUCT = ScalarVariable<"STRUCT", details::struct_scalar_constraint, EqualityComparable, Hashable>;

namespace details {

template <typename T> struct is_scalar_variable : std::false_type {};

template <util::compile_time_string Name, typename... Constraints>
struct is_scalar_variable<ScalarVariable<Name, Constraints...>> : std::true_type {};

template <typename T>
concept scalar_variable = is_scalar_variable<std::remove_cvref_t<T>>::value;

template <typename T> inline constexpr bool generic_scalar = scalar_variable<T>;

template <typename T>
concept scalar_property_constraint = std::same_as<T, EqualityComparable> || std::same_as<T, Hashable>;

template <typename Variable, typename Property> struct scalar_variable_has_property : std::false_type {};

template <util::compile_time_string Name, typename... Constraints, typename Property>
struct scalar_variable_has_property<ScalarVariable<Name, Constraints...>, Property>
    : std::bool_constant<(std::same_as<Constraints, Property> || ...)> {};

template <typename Variable>
inline constexpr bool scalar_variable_supports_equality =
    scalar_variable_has_property<std::remove_cvref_t<Variable>, EqualityComparable>::value;

template <typename Variable>
inline constexpr bool scalar_variable_supports_hash =
    scalar_variable_has_property<std::remove_cvref_t<Variable>, Hashable>::value;

} // namespace details

} // namespace bg
