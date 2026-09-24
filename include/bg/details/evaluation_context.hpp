#pragma once

namespace bg::details {

struct EvaluationContextSection {
    const EvaluationContextSection *parent;
    const void *type;
};

} // namespace bg::details
