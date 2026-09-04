#pragma once

#include <bg/node/aggregate_node_template.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/node/state.hpp>
#include <bg/time_series/tsd/tsd_wiring.hpp>

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

} // namespace bg
