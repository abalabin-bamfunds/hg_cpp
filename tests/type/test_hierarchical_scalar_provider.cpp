#include "test_hierarchical_scalar_provider.hpp"

#include <hg/scalar/bundle/scalar_type.hpp>

namespace test_hierarchical_scalar_provider {

namespace {

template <template <typename, auto...> typename Field> struct ProviderOnlyField {
    Field<hg::i64> value;
};

template <template <typename, auto...> typename Field> struct ConflictingInstrument {
    Field<hg::symbol> symbol;
    Field<double, hg::optional> price;
    Field<hg::Bundle<ProviderOnlyField>> extra;
};

} // namespace

const hg::ScalarType &instrument_type() {
    return hg::cpp_scalar_type<hg::Bundle<Instrument>>();
}

const hg::ScalarType &recursive_instrument_type() {
    return hg::cpp_scalar_type<hg::Bundle<RecursiveInstrument>>();
}

const hg::ScalarType &conflicting_instrument_type() {
    (void)instrument_type();
    return hg::details::canonical_hierarchical_scalar_type(
        typeid(Instrument<hg::details::schema_identity_field>),
        typeid(ConflictingInstrument<hg::details::schema_identity_field>),
        "Instrument",
        &hg::details::initialize_hierarchical_scalar_type<ConflictingInstrument>);
}

} // namespace test_hierarchical_scalar_provider
