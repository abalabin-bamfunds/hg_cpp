#pragma once

#include <hg/scalar/data.hpp>
#include <hg/scalar/storage_traits.hpp>

namespace hg::details {

template <typename T> typename scalar_storage_traits<T>::value_type scalar_data_value(const ScalarData &source);

} // namespace hg::details
