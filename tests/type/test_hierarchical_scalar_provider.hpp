#pragma once

#include <hg/optionality.hpp>
#include <hg/scalar/builtins.hpp>
#include <hg/scalar/bundle/type.hpp>
#include <hg/type/scalar_type.hpp>

namespace test_hierarchical_scalar_provider {

template <template <typename, auto...> typename Field> struct Instrument {
    Field<hg::symbol> symbol;
    Field<double, hg::optional> price;
};

template <template <typename, auto...> typename Field> struct RecursiveInstrument {
    Field<hg::symbol> symbol;
    Field<hg::Bundle<RecursiveInstrument>, hg::optional> related;
};

const hg::ScalarType &instrument_type();
const hg::ScalarType &recursive_instrument_type();
const hg::ScalarType &conflicting_instrument_type();

} // namespace test_hierarchical_scalar_provider
