#pragma once

#include <bg/util/compile_time_string.hpp>

#include <type_traits>

namespace bg {

template <util::compile_time_string Name> struct SizeVariableDeclaration {
    static constexpr auto name = Name;
};

inline constexpr auto SIZE = SizeVariableDeclaration<"SIZE">{};

namespace details {

template <typename T> struct is_size_variable_declaration : std::false_type {};

template <util::compile_time_string Name>
struct is_size_variable_declaration<SizeVariableDeclaration<Name>> : std::true_type {};

template <typename T>
concept size_variable_declaration = is_size_variable_declaration<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
