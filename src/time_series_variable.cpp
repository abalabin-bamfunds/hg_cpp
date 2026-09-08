#include <bg/type/time_series_variable.hpp>

#include <bg/type/type_resolution_error.hpp>

namespace bg {

bool GenericTimeSeriesVariableType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *time_series = dynamic_cast<const TimeSeriesType *>(&supplied);
    if (time_series == nullptr || !time_series->is_resolved()) {
        return false;
    }

    const auto &structure = time_series->with_guarantee(TimeSeriesGuarantee::ordinary);
    auto trial = type_map.overlay();
    if (!variable_->match(structure, trial) || !guarantee().match(*time_series->guarantee().value(), trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesVariableType::resolve(const TypeMap &type_map) const {
    const auto &resolved = variable_->resolve(type_map);
    const auto *time_series = dynamic_cast<const TimeSeriesType *>(&resolved);
    if (time_series == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"time series variable"},
                                  "time series variable resolved outside its category"};
    }

    return time_series->with_guarantee(guarantee().resolve(type_map));
}

} // namespace bg
