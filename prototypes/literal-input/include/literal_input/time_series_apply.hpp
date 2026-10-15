#pragma once

#include <bg/literal/scalar_literal.hpp>
#include <bg/time_series/time_series_output.hpp>

namespace literal {

template <typename Variable>
void apply(const bg::Out<Variable> &output, const ScalarLiteral &literal) {
    output.apply(literal);
}

} // namespace literal
