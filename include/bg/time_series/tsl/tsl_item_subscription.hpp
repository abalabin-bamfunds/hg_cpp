#pragma once

#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstddef>

namespace bg::details {

struct TslItemReferenceInput {
    const EvaluationTime *last_updated;
    const TimeSeriesRuntimeType *runtime_type;
    ReferenceBinding binding;
    bool modified;
};

[[nodiscard]] TslItemReferenceInput tsl_item_reference_input(Reference tsl, std::size_t index);
const TimeSeriesRuntimeType &tsl_item_reference_runtime_type(const TimeSeriesRuntimeType &tsl);

} // namespace bg::details
