#pragma once

#include <bg/optionality.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/type/scalar_type.hpp>

namespace test_hierarchical_scalar_provider {

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
};

const bg::ScalarType &instrument_type();

} // namespace test_hierarchical_scalar_provider
