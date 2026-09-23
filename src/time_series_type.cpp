#include <bg/type/time_series_type.hpp>

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>

namespace bg {
namespace {

bool item_requires_reference_representation(const TimeSeriesType &requested, const TimeSeriesType &supplied) noexcept {
    const auto *requested_reference = type_cast<TimeSeriesReferenceType>(&requested);
    const auto *supplied_reference = type_cast<TimeSeriesReferenceType>(&supplied);
    if ((requested_reference == nullptr) != (supplied_reference == nullptr)) {
        return true;
    }

    return requested.target_type().requires_reference_representation_for(supplied.target_type());
}

} // namespace

const TimeSeriesType &TimeSeriesType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    if (this->guarantee().value() == guarantee) {
        return *this;
    }

    throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"guarantee"},
                              "an unresolved time series type cannot change guarantee"};
}

const TimeSeriesType *TimeSeriesType::common_type(const TimeSeriesType &other) const {
    const auto own_guarantee = guarantee().value();
    const auto other_guarantee = other.guarantee().value();
    if (!is_resolved() || !other.is_resolved() || !own_guarantee || !other_guarantee) {
        return nullptr;
    }

    const auto &own_shape = with_guarantee(TimeSeriesGuarantee::ordinary);
    const auto &other_shape = other.with_guarantee(TimeSeriesGuarantee::ordinary);
    if (&own_shape != &other_shape) {
        return nullptr;
    }

    return &own_shape.with_guarantee(weakest(*own_guarantee, *other_guarantee));
}

bool FixedItemTimeSeriesType::requires_reference_representation_for(const TimeSeriesType &supplied) const noexcept {
    const auto *collection = dynamic_cast<const FixedItemTimeSeriesType *>(&supplied);
    if (collection == nullptr) {
        return false;
    }

    const auto count = std::min(item_count(), collection->item_count());
    for (auto index = std::size_t{}; index < count; ++index) {
        if (item_requires_reference_representation(item_type(index), collection->item_type(index))) {
            return true;
        }
    }
    return false;
}

} // namespace bg
