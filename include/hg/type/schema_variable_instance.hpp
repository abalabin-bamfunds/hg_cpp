#pragma once

#include <hg/time_series/schema_variable.hpp>
#include <hg/type/schema.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/type/type_variable.hpp>
#include <hg/util/static_storage.hpp>

namespace hg::details {

template <util::compile_time_string Name>
const TypeVariableType<SchemaType> &schema_variable_instance() {
    static const auto variable = util::static_storage<TypeVariableType<SchemaType>>{util::symbol{Name.view()}};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(variable.get());
    return variable.get();
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

} // namespace hg::details

namespace hg {

template <template <template <typename, auto...> typename> typename Schema>
const SchemaType &schema_type<Schema>::type() {
    if constexpr (details::schema_variable_template<Schema>) {
        return details::schema_variable_instance<Schema>();
    } else {
        return details::time_series_schema_type<Schema>();
    }
}

} // namespace hg
