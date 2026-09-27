#include "test_hierarchical_scalar_provider.hpp"

#include <bg/scalar/bundle/scalar_type.hpp>

namespace test_hierarchical_scalar_provider {

const bg::ScalarType &instrument_type() {
    return bg::cpp_scalar_type<bg::Bundle<Instrument>>();
}

} // namespace test_hierarchical_scalar_provider
