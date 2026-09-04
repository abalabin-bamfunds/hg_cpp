#pragma once

#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/type_variable.hpp>

namespace bg::details {

template <typename Scope, template <template <typename> typename> typename Declaration>
    requires bundle_schema_variable_declaration<Declaration<bundle_schema_field>>
[[nodiscard]] const TypeVariable<BundleSchemaType> &schema_variable_instance() {
    using Variable = Declaration<bundle_schema_field>;
    static const auto variable = TypeVariable<BundleSchemaType>{util::symbol{Variable::name.view()}};
    return variable;
}

} // namespace bg::details
