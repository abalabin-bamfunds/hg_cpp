#pragma once

namespace hg::details {

struct EvaluationContextSection {
    const EvaluationContextSection *parent;
    const void *type;
};

} // namespace hg::details
