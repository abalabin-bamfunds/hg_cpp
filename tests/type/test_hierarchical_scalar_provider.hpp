#pragma once

#include <bg/optionality.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/type/scalar_type.hpp>

namespace test_hierarchical_scalar_provider {

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
};

template <template <typename, auto...> typename Field> struct RecursiveInstrument {
    Field<bg::symbol> symbol;
    Field<bg::Bundle<RecursiveInstrument>, bg::optional> related;
};

const bg::ScalarType &instrument_type();
const bg::ScalarType &recursive_instrument_type();
const bg::ScalarType &conflicting_instrument_type();

} // namespace test_hierarchical_scalar_provider
