#pragma once

#include <cstddef>

namespace bg::details {

struct EvaluationContextSection {
    const EvaluationContextSection *parent;
    const void *type;
};

struct PendingEvaluation {
    const void *target{};
    std::size_t token{};
};

} // namespace bg::details
