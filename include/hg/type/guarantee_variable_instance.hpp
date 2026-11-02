#pragma once

#include <hg/time_series/guarantee_variable.hpp>
#include <hg/type/guarantee_parameter.hpp>
#include <hg/type/variable_registry.hpp>
#include <hg/util/static_storage.hpp>
#include <hg/util/symbol.hpp>

#include <type_traits>

namespace hg::details {

template <typename Declaration>
    requires guarantee_variable_declaration<Declaration>
const GuaranteeVariable &guarantee_variable_instance() {
    using Variable = std::remove_cv_t<Declaration>;
    static const auto &variable = VariableRegistry::instance().get_or_create_static<
        Variable, GuaranteeVariable>([&]() -> const auto & {
            static const auto variable = util::static_storage<GuaranteeVariable>{util::symbol{Variable::name.view()},
                                                                                  Variable::minimum};
            return variable.get();
        });

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

} // namespace hg::details
