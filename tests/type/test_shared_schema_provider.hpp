#pragma once

#include <bg/scalar/builtins.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/time_series/tsb/tsb.hpp>

namespace test_shared_schema_provider {

template <template <typename, auto...> typename Field> struct SharedSchema {
    Field<bg::i64> count;
    Field<double> value;
};

const bg::ScalarType &scalar_type();
const bg::TimeSeriesBundleType &time_series_type();

} // namespace test_shared_schema_provider
