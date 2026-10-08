#pragma once

#include <bg/time_series/schema_variable.hpp>
#include <bg/type/schema.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_variable.hpp>

namespace bg::details {

template <util::compile_time_string Name>
const TypeVariableType<SchemaType> &schema_variable_instance() {
    static const auto variable = TypeVariableType<SchemaType>{util::symbol{Name.view()}};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(variable);
    return variable;
}

template <typename Declaration>
    requires schema_variable_declaration<Declaration>
const TypeVariableType<SchemaType> &schema_variable_instance() {
    using Variable = std::remove_cv_t<Declaration>;
    return schema_variable_instance<Variable::name>();
}

template <template <template <typename, auto...> typename> typename Declaration>
    requires schema_variable_template<Declaration>
const TypeVariableType<SchemaType> &schema_variable_instance() {
    return schema_variable_instance<Declaration<schema_variable_field>>();
}

template <template <template <typename, auto...> typename> typename Declaration>
const SchemaType &time_series_schema_type();

} // namespace bg::details

namespace bg {

template <template <template <typename, auto...> typename> typename Schema>
const SchemaType &schema_type<Schema>::type() {
    if constexpr (details::schema_variable_template<Schema>) {
        return details::schema_variable_instance<Schema>();
    } else {
        return details::time_series_schema_type<Schema>();
    }
}

} // namespace bg
