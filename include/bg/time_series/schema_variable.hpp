#pragma once

#include <bg/util/compile_time_string.hpp>

#include <type_traits>

namespace bg {

class BundleSchemaType;

template <util::compile_time_string Name, template <typename> typename Field> struct BundleSchemaVariableDeclaration {
    static constexpr auto name = Name;
};

template <template <typename> typename Field> using SCHEMA = BundleSchemaVariableDeclaration<"SCHEMA", Field>;

template <template <template <typename> typename> typename Schema>
struct schema_type {
    static const BundleSchemaType &type();
};

namespace details {

template <typename T> struct is_bundle_schema_variable_declaration : std::false_type {};

template <util::compile_time_string Name, template <typename> typename Field>
struct is_bundle_schema_variable_declaration<BundleSchemaVariableDeclaration<Name, Field>> : std::true_type {};

template <typename T>
concept bundle_schema_variable_declaration = is_bundle_schema_variable_declaration<std::remove_cv_t<T>>::value;

template <typename T> struct is_schema_type : std::false_type {};

template <template <template <typename> typename> typename Schema>
struct is_schema_type<schema_type<Schema>> : std::true_type {};

template <typename T>
concept schema_type_declaration = is_schema_type<std::remove_cv_t<T>>::value;

template <typename T> struct schema_type_traits;

template <template <template <typename> typename> typename Schema> struct schema_type_traits<schema_type<Schema>> {
    template <template <typename> typename Field> using declaration = Schema<Field>;
};

} // namespace details

} // namespace bg
