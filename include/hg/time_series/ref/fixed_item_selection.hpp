#pragma once

#include <hg/time_series/time_series_state.hpp>
#include <hg/time_series/item_id.hpp>
#include <hg/time_series/ref/dynamic_reference_input.hpp>
#include <hg/time_series/ref/ref_provider.hpp>
#include <hg/time_series/time_series_type.hpp>

namespace hg::details {

struct FixedItemReferenceInput {
    const TimeSeriesState *last_updated;
    const TimeSeriesRuntimeType *runtime_type;
    ReferenceBinding binding;
    bool modified;
};

[[nodiscard]] FixedItemReferenceInput fixed_item_reference_input(Reference collection, item_id_t id);
const TimeSeriesRuntimeType &fixed_item_reference_runtime_type(const TimeSeriesType &item_type);

} // namespace hg::details
