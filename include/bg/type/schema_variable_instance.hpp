#pragma once

#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/type_variable.hpp>

namespace bg::details {

template <typename Declaration>
    requires bundle_schema_variable_declaration<Declaration>
const TypeVariableType<BundleSchemaType> &schema_variable_instance() {
    using Variable = std::remove_cv_t<Declaration>;
    static const auto variable = TypeVariableType<BundleSchemaType>{util::symbol{Variable::name.view()}};

    return variable;
}

template <template <template <typename> typename> typename Declaration>
    requires bundle_schema_variable_declaration<Declaration<bundle_schema_field>>
const TypeVariableType<BundleSchemaType> &schema_variable_instance() {
    return schema_variable_instance<Declaration<bundle_schema_field>>();
}

} // namespace bg::details
