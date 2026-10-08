#include "test_shared_schema_provider.hpp"

namespace test_shared_schema_provider {

const bg::ScalarType &scalar_type() {
    return bg::cpp_scalar_type<bg::Bundle<SharedSchema>>();
}

const bg::TimeSeriesBundleType &time_series_type() {
    return bg::TSB<SharedSchema>::type();
}

} // namespace test_shared_schema_provider
