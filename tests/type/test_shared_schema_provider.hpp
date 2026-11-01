#pragma once

#include <hg/scalar/builtins.hpp>
#include <hg/scalar/bundle/scalar_type.hpp>
#include <hg/time_series/tsb/tsb.hpp>

namespace test_shared_schema_provider {

template <template <typename, auto...> typename Field> struct SharedSchema {
    Field<hg::i64> count;
    Field<double> value;
};

const hg::ScalarType &scalar_type();
const hg::TimeSeriesBundleType &time_series_type();

} // namespace test_shared_schema_provider
