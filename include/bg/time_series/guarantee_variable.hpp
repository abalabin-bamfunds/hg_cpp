#pragma once

#include <bg/util/compile_time_string.hpp>

#include <type_traits>

namespace bg {

template <util::compile_time_string Name> struct GuaranteeVariableDeclaration {
    static constexpr auto name = Name;
};

template <util::compile_time_string Name>
inline constexpr auto guarantee_variable = GuaranteeVariableDeclaration<Name>{};

namespace details {

template <typename T> struct is_guarantee_variable_declaration : std::false_type {};

template <util::compile_time_string Name>
struct is_guarantee_variable_declaration<GuaranteeVariableDeclaration<Name>> : std::true_type {};

template <typename T>
concept guarantee_variable_declaration = is_guarantee_variable_declaration<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
