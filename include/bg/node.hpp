#pragma once

#include <bg/graph/wiring_arguments.hpp>
#include <bg/node/aggregate_node_template.hpp>
#include <bg/node/details/callable_node.hpp>
#include <bg/node/details/callable_node_options.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/node/state.hpp>
#include <bg/type/type_parameter.hpp>

#include <utility>

namespace bg::details {

template <typename... Declaration> struct aggregate_node_declaration {
    using Parsed = type_parameterized_declaration<Declaration...>;
    using Types = typename Parsed::Types;
    using Definition = typename Parsed::Definition;

    static constexpr auto valid = [] {
        if constexpr (type_parameter_declaration<Definition>) {
            return false;
        } else {
            return AggregateNodeClass<Definition> && Parsed::has_valid_parameters;
        }
    }();
};

template <typename... Declaration, std::size_t... Index>
[[nodiscard]] auto make_aggregate_node(std::index_sequence<Index...>) {
    using Parts = aggregate_node_declaration<Declaration...>;
    return AggregateNodeTemplate<typename Parts::Definition, default_wiring_label,
                                 std::tuple_element_t<Index, typename Parts::Types>...>{};
}

template <typename... Declaration, typename Label, std::size_t... Index>
[[nodiscard]] auto make_aggregate_node(Label &&label, std::index_sequence<Index...>) {
    using Parts = aggregate_node_declaration<Declaration...>;
    return AggregateNodeTemplate<typename Parts::Definition, std::decay_t<Label>,
                                 std::tuple_element_t<Index, typename Parts::Types>...>{
        std::forward<Label>(label)};
}

} // namespace bg::details

namespace bg {

template <typename... Declaration>
    requires(sizeof...(Declaration) != 0 && details::aggregate_node_declaration<Declaration...>::valid)
[[nodiscard]] auto node() {
    return details::make_aggregate_node<Declaration...>(
        std::make_index_sequence<sizeof...(Declaration) - 1>{});
}

template <typename... Declaration, typename Label>
    requires(sizeof...(Declaration) != 0 && details::aggregate_node_declaration<Declaration...>::valid)
[[nodiscard]] auto node(Label &&label) {
    return details::make_aggregate_node<Declaration...>(
        std::forward<Label>(label), std::make_index_sequence<sizeof...(Declaration) - 1>{});
}

template <typename... TypeParameters, details::runtime_signature_node Definition>
    requires(details::type_parameter_declaration<TypeParameters> && ...)
[[nodiscard]] auto node(Definition definition) {
    return RuntimeSignatureNodeTemplate<Definition, TypeParameters...>{std::move(definition)};
}

template <typename... TypeParameters, details::callable_node Function>
    requires(details::type_parameter_declaration<TypeParameters> && ...)
[[nodiscard]] auto node(Function function) {
    return RuntimeSignatureNodeTemplate<details::CallableNodeDefinition<Function>, TypeParameters...>{
        details::CallableNodeDefinition<Function>{std::move(function)}};
}

template <typename... TypeParameters, details::callable_node Function, typename... Options>
    requires((details::type_parameter_declaration<TypeParameters> && ...) &&
             details::callable_node_options<Options...>)
[[nodiscard]] auto node(details::CallableEvaluation<Function> evaluation, Options... options) {
    using Definition = details::CallableNodeDefinition<Function, Options...>;
    return RuntimeSignatureNodeTemplate<Definition, TypeParameters...>{
        Definition{std::move(evaluation.function), std::move(options)...}};
}

} // namespace bg
