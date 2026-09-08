#pragma once

#include <bg/time_series/size_variable.hpp>
#include <bg/type/size_parameter.hpp>

namespace bg::details {

template <typename Declaration>
    requires size_variable_declaration<Declaration>
const SizeVariable &size_variable_instance() {
    using Variable = std::remove_cv_t<Declaration>;
    static const auto variable = SizeVariable{util::symbol{Variable::name.view()}};

    return variable;
}

template <auto Declaration>
    requires size_variable_declaration<decltype(Declaration)>
const SizeVariable &size_variable_instance() {
    return size_variable_instance<std::remove_cv_t<decltype(Declaration)>>();
}

template <auto Size> [[nodiscard]] SizeParameter size_parameter() {
    if constexpr (size_variable_declaration<decltype(Size)>) {
        return SizeParameter{size_variable_instance<Size>()};
    } else {
        return SizeParameter{Size};
    }
}

} // namespace bg::details
