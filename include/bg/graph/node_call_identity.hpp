#pragma once

#include <cstddef>

namespace bg {

// Identifies a collected node call before GraphWiring binds it to a concrete node.
struct NodeCallIdentity {
    std::size_t value;

    friend bool operator==(const NodeCallIdentity &, const NodeCallIdentity &) = default;
};

} // namespace bg
