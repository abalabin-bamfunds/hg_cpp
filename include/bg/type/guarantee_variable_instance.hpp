#pragma once

#include <bg/time_series/guarantee_variable.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/symbol.hpp>

#include <type_traits>

namespace bg::details {

template <typename Declaration>
    requires guarantee_variable_declaration<Declaration>
const GuaranteeVariable &guarantee_variable_instance() {
    using Variable = std::remove_cv_t<Declaration>;
    static const auto variable = GuaranteeVariable{util::symbol{Variable::name.view()}};
    [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(variable);

    return variable;
}

template <auto Declaration>
    requires guarantee_variable_declaration<decltype(Declaration)>
const GuaranteeVariable &guarantee_variable_instance() {
    return guarantee_variable_instance<std::remove_cv_t<decltype(Declaration)>>();
}

template <auto Guarantee> [[nodiscard]] GuaranteeParameter guarantee_parameter() {
    if constexpr (std::same_as<std::remove_cv_t<decltype(Guarantee)>, TimeSeriesGuarantee>) {
        return GuaranteeParameter{Guarantee};
    } else {
        return GuaranteeParameter{guarantee_variable_instance<Guarantee>()};
    }
}

} // namespace bg::details
