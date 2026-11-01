#pragma once

#include <hg/literal/scalar_literal.hpp>
#include <hg/time_series/time_series_output.hpp>

namespace literal {

template <typename Variable>
void apply(const hg::Out<Variable> &output, const ScalarLiteral &literal) {
    output.apply(literal);
}

} // namespace literal
