#pragma once

#include <cstddef>

namespace bg {

// Reports which inner graph became runnable to its owning node.
struct RuntimeGraphSchedulingHandler {
    using token_type = std::size_t;

    // The receiver remains valid until every graph using this evaluator stops.
    void *receiver;
    void (*inner_scheduled)(void *receiver, token_type token) noexcept;
};

} // namespace bg
