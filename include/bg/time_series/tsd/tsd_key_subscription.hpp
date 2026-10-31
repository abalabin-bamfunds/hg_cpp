#pragma once

#include <bg/time_series/time_series_state.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstdint>

namespace bg::details {

struct TsdItemReferenceInput {
    const TimeSeriesState *last_updated;
    const TimeSeriesRuntimeType *runtime_type;
    ReferenceBinding binding;
    bool modified;
};

[[nodiscard]] TsdItemReferenceInput tsd_item_reference_input(Reference tsd, Scalar<> key);
const TimeSeriesRuntimeType &tsd_item_reference_runtime_type(const TimeSeriesRuntimeType &tsd);
} // namespace bg::details
