#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstdint>

namespace bg::details {

struct TsdItemReferenceInput {
    const EvaluationTime *last_updated;
    const TimeSeriesRuntimeType *runtime_type;
    const void *value;
    bool modified;
};

[[nodiscard]] TsdItemReferenceInput tsd_item_reference_input(Reference tsd, Scalar<> key);
const TimeSeriesRuntimeType &tsd_item_reference_runtime_type(const TimeSeriesRuntimeType &tsd);

} // namespace bg::details
