#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/time_series/dynamic_value_input.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

namespace bg::details {

[[nodiscard]] DynamicValueBinding<bool> tss_membership_binding(Reference collection, Scalar<> key);
[[nodiscard]] DynamicValueBinding<bool> tsd_membership_binding(Reference collection, Scalar<> key,
                                                               bool valid_items);

} // namespace bg::details
