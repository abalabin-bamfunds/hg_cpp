#pragma once

#include <bg/scalar/data.hpp>
#include <bg/scalar/storage_traits.hpp>

namespace bg::details {

template <typename T> typename scalar_storage_traits<T>::value_type scalar_data_value(const ScalarData &source);

} // namespace bg::details
