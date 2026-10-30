#include "test_hierarchical_scalar_provider.hpp"

#include <bg/scalar/bundle/scalar_type.hpp>

namespace test_hierarchical_scalar_provider {

namespace {

template <template <typename, auto...> typename Field> struct ProviderOnlyField {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field> struct ConflictingInstrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
    Field<bg::Bundle<ProviderOnlyField>> extra;
};

} // namespace

const bg::ScalarType &instrument_type() {
    return bg::cpp_scalar_type<bg::Bundle<Instrument>>();
}

const bg::ScalarType &recursive_instrument_type() {
    return bg::cpp_scalar_type<bg::Bundle<RecursiveInstrument>>();
}

const bg::ScalarType &conflicting_instrument_type() {
    (void)instrument_type();
    return bg::details::canonical_hierarchical_scalar_type(
        typeid(Instrument<bg::details::schema_identity_field>),
        typeid(ConflictingInstrument<bg::details::schema_identity_field>),
        "Instrument",
        &bg::details::initialize_hierarchical_scalar_type<ConflictingInstrument>);
}

} // namespace test_hierarchical_scalar_provider
