#pragma once

#include <bg/scalar/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/symbol.hpp>

namespace bg::details {

template <typename Declaration> struct ScalarVariableInstance;

template <util::compile_time_string Name, typename... Constraints>
struct ScalarVariableInstance<ScalarVariable<Name, Constraints...>> {
    static const TypeVariable<ScalarType> &get() {
        static const auto variable = [] {
            auto alternatives = std::vector<const ScalarType *>{};
            auto property_constraints = std::vector<TypeVariable<ScalarType>::Constraint>{};

            (
                [&] {
                    if constexpr (std::same_as<Constraints, EqualityComparable>) {
                        property_constraints.push_back(&supports_scalar_equality);
                    } else if constexpr (std::same_as<Constraints, Hashable>) {
                        property_constraints.push_back(&supports_scalar_hash);
                    } else {
                        alternatives.push_back(&cpp_scalar_type<Constraints>());
                    }
                }(),
                ...);

            return TypeVariable<ScalarType>{util::symbol{Name.view()}, std::move(alternatives),
                                            std::move(property_constraints)};
        }();

        return variable;
    }
};

template <scalar_variable Declaration> const TypeVariable<ScalarType> &scalar_variable_instance() {
    return ScalarVariableInstance<Declaration>::get();
}

template <typename Declaration> decltype(auto) scalar_type_access<Declaration>::type() {
    static_assert(scalar_variable<Declaration>, "unsupported scalar declaration");
    return scalar_variable_instance<Declaration>();
}

template <util::compile_time_string Name, typename... Constraints>
struct declaration_variable_traits<ScalarVariable<Name, Constraints...>> {
    using Declaration = ScalarVariable<Name, Constraints...>;

    template <typename Visitor> static void for_each(Visitor &&visitor) {
        std::forward<Visitor>(visitor).template operator()<Declaration>(scalar_variable_instance<Declaration>());
    }
};

template <scalar_variable Declaration> struct scalar_type_traits<Declaration> : scalar_type_properties {
    static constexpr bool supports_implicit_argument = false;

    static const ScalarType &get() { return scalar_variable_instance<Declaration>(); }
};

} // namespace bg::details
