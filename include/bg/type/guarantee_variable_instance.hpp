#pragma once

#include <bg/time_series/guarantee_variable.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/util/symbol.hpp>

#include <type_traits>

namespace bg::details {

template <typename Scope, auto Declaration>
    requires guarantee_variable_declaration<decltype(Declaration)>
[[nodiscard]] const GuaranteeVariable &guarantee_variable_instance() {
    using Variable = std::remove_cv_t<decltype(Declaration)>;
    static const auto variable = GuaranteeVariable{util::symbol{Variable::name.view()}};

    return variable;
}

template <typename Scope, auto Guarantee> [[nodiscard]] GuaranteeParameter guarantee_parameter() {
    if constexpr (std::same_as<std::remove_cv_t<decltype(Guarantee)>, TimeSeriesGuarantee>) {
        return GuaranteeParameter{Guarantee};
    } else {
        return GuaranteeParameter{guarantee_variable_instance<Scope, Guarantee>()};
    }
}

} // namespace bg::details
