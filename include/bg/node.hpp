#pragma once

#include <bg/node/aggregate_node_template.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/state.hpp>

namespace bg {

template <AggregateNodeClass Definition> [[nodiscard]] AggregateNodeTemplate<Definition> node() {
    return AggregateNodeTemplate<Definition>{};
}

template <AggregateNodeClass Definition, typename Label> [[nodiscard]] auto node(Label &&label) {
    return AggregateNodeTemplate<Definition, std::decay_t<Label>>{std::forward<Label>(label)};
}

} // namespace bg
