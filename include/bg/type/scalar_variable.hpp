#pragma once

#include <bg/scalar/type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/symbol.hpp>

namespace bg::details {

template <typename Scope, typename Declaration> struct ScalarVariableInstance;

template <typename Scope, util::compile_time_string Name, typename... Constraints>
struct ScalarVariableInstance<Scope, ScalarVariable<Name, Constraints...>> {
    [[nodiscard]] static const TypeVariable<ScalarType> &get() {
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

template <typename Scope, scalar_variable Declaration>
[[nodiscard]] const TypeVariable<ScalarType> &scalar_variable_instance() {
    return ScalarVariableInstance<Scope, Declaration>::get();
}

template <scalar_variable Declaration> struct scalar_type_traits<Declaration> : scalar_type_properties {
    static constexpr bool supports_implicit_argument = false;

    template <typename Scope> [[nodiscard]] static const ScalarType &get() {
        return scalar_variable_instance<Scope, Declaration>();
    }
};

} // namespace bg::details
