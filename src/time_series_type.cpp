#include <bg/type/time_series_type.hpp>

#include <bg/type/type_resolution_error.hpp>

namespace bg {

const TimeSeriesType &TimeSeriesType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    if (this->guarantee().value() == guarantee) {
        return *this;
    }

    throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"guarantee"},
                              "an unresolved time series type cannot change guarantee"};
}

} // namespace bg
