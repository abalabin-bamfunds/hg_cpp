#pragma once

#include <bg/graph/wiring_arguments.hpp>
#include <bg/node/aggregate_node_template.hpp>
#include <bg/node/details/callable_node.hpp>
#include <bg/node/details/callable_node_options.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/node/state.hpp>
#include <bg/time_series/tsd/tsd_wiring.hpp>

#include <utility>

namespace bg {

template <AggregateNodeClass Definition> [[nodiscard]] AggregateNodeTemplate<Definition> node() {
    return AggregateNodeTemplate<Definition>{};
}

template <AggregateNodeClass Definition, typename Label> [[nodiscard]] auto node(Label &&label) {
    return AggregateNodeTemplate<Definition, std::decay_t<Label>>{std::forward<Label>(label)};
}

template <details::runtime_signature_node Definition> [[nodiscard]] auto node(Definition definition) {
    return RuntimeSignatureNodeTemplate<Definition>{std::move(definition)};
}

template <details::callable_node Function> [[nodiscard]] auto node(Function function) {
    return RuntimeSignatureNodeTemplate<details::CallableNodeDefinition<Function>>{
        details::CallableNodeDefinition<Function>{std::move(function)}};
}

template <details::callable_node Function, typename... Options>
    requires details::callable_node_options<Options...>
[[nodiscard]] auto node(details::CallableEvaluation<Function> evaluation, Options... options) {
    using Definition = details::CallableNodeDefinition<Function, Options...>;
    return RuntimeSignatureNodeTemplate<Definition>{Definition{std::move(evaluation.function), std::move(options)...}};
}

} // namespace bg
