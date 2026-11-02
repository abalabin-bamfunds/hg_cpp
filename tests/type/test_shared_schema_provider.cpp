#include "test_shared_schema_provider.hpp"

namespace test_shared_schema_provider {

const hg::ScalarType &scalar_type() {
    return hg::cpp_scalar_type<hg::Bundle<SharedSchema>>();
}

const hg::TimeSeriesBundleType &time_series_type() {
    return hg::TSB<SharedSchema>::type();
}

} // namespace test_shared_schema_provider
