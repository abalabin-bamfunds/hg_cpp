#pragma once

#include <bg/util/compile_time_string.hpp>

#include <type_traits>

namespace bg {

template <util::compile_time_string Name, template <typename> typename Field> struct BundleSchemaVariableDeclaration {
    static constexpr auto name = Name;
};

template <template <typename> typename Field> using SCHEMA = BundleSchemaVariableDeclaration<"SCHEMA", Field>;

namespace details {

template <typename T> struct is_bundle_schema_variable_declaration : std::false_type {};

template <util::compile_time_string Name, template <typename> typename Field>
struct is_bundle_schema_variable_declaration<BundleSchemaVariableDeclaration<Name, Field>> : std::true_type {};

template <typename T>
concept bundle_schema_variable_declaration = is_bundle_schema_variable_declaration<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
