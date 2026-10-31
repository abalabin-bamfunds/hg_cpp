#pragma once

#include <bg/util/compile_time_string.hpp>

#include <type_traits>

namespace bg {

class SchemaType;

template <util::compile_time_string Name>
struct SchemaVariableDeclaration {
    static constexpr auto name = Name;
};

template <util::compile_time_string Name, template <typename, auto...> typename Field>
using BundleSchemaVariableDeclaration = SchemaVariableDeclaration<Name>;

template <template <typename, auto...> typename Field>
using SCHEMA = BundleSchemaVariableDeclaration<"SCHEMA", Field>;

template <template <template <typename, auto...> typename> typename Schema>
struct schema_type {
    static const SchemaType &type();
};

namespace details {

template <typename, auto...> struct schema_variable_field {};

template <typename T> struct is_schema_variable_declaration : std::false_type {};

template <util::compile_time_string Name>
struct is_schema_variable_declaration<SchemaVariableDeclaration<Name>> : std::true_type {};

template <typename T>
concept schema_variable_declaration = is_schema_variable_declaration<std::remove_cv_t<T>>::value;

template <typename T>
concept bundle_schema_variable_declaration = schema_variable_declaration<T>;

template <typename T> struct is_schema_type : std::false_type {};

template <template <template <typename, auto...> typename> typename Schema>
struct is_schema_type<schema_type<Schema>> : std::true_type {};

template <typename T>
concept schema_type_declaration = is_schema_type<std::remove_cv_t<T>>::value;

template <typename T> struct schema_type_traits;

template <template <template <typename, auto...> typename> typename Schema>
struct schema_type_traits<schema_type<Schema>> {
    template <template <typename, auto...> typename Field> using declaration = Schema<Field>;
};

template <template <template <typename, auto...> typename> typename Schema>
concept schema_variable_template = schema_variable_declaration<Schema<schema_variable_field>>;

} // namespace details

} // namespace bg
