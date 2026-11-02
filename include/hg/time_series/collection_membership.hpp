#pragma once

#include <hg/scalar/scalar.hpp>
#include <hg/time_series/dynamic_value_input.hpp>
#include <hg/time_series/ref/ref_storage.hpp>
#include <hg/time_series/time_series_runtime_type.hpp>

namespace hg::details {

[[nodiscard]] DynamicValueBinding<bool> tss_membership_binding(Reference collection, Scalar<> key);
[[nodiscard]] DynamicValueBinding<bool> tsd_membership_binding(Reference collection, Scalar<> key,
                                                               bool valid_items);

} // namespace hg::details
