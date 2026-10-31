#pragma once

#include <bg/node/details/resolver.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/details/time_series_type.hpp>

#include <algorithm>
#include <vector>

namespace bg::details {

template <typename Definition, typename Declaration>
void add_resolver(std::vector<VariableResolver> &resolvers, const Variable &variable) {
    if (std::ranges::any_of(resolvers, [&](const auto &resolver) { return resolver.variable == &variable; })) {
        return;
    }
    if constexpr (requires(const WiringArguments &arguments) { Definition::resolve(Declaration{}, arguments); }) {
        resolvers.emplace_back(&variable, [&variable](const TypeMap &, const WiringArguments &arguments) {
            return resolver_mapping(Definition::resolve(Declaration{}, arguments), variable);
        });
    }
}

template <typename Definition, typename Declaration>
void append_aggregate_resolvers(std::vector<VariableResolver> &resolvers) {
    for_each_declaration_variable<Declaration>([&]<typename VariableDeclaration>(const Variable &variable) {
        add_resolver<Definition, VariableDeclaration>(resolvers, variable);
    });
}

} // namespace bg::details
