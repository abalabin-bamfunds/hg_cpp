#include <bg/type/time_series_variable.hpp>

#include <bg/type/type_resolution_error.hpp>

namespace bg {

bool GenericTimeSeriesVariableType::do_match(const Type &supplied, TypeMap &type_map) const {
    return do_match(supplied, type_map, std::nullopt);
}

bool GenericTimeSeriesVariableType::do_match(const Type &supplied, TypeMap &type_map,
                                             std::optional<TimeSeriesGuarantee> reference_limit) const {
    const auto *time_series = type_cast<TimeSeriesType>(&supplied);
    if (time_series == nullptr || !time_series->is_resolved()) {
        return false;
    }

    const auto &structure = time_series->with_guarantee(TimeSeriesGuarantee::ordinary);
    const auto supplied_guarantee = *time_series->guarantee().value();
    const auto observed_guarantee = reference_limit ? weakest(supplied_guarantee, *reference_limit)
                                                    : supplied_guarantee;
    auto trial = type_map.overlay();
    if (!variable_->match(structure, trial, reference_limit) || !guarantee().match(observed_guarantee, trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesVariableType::resolve(const TypeMap &type_map) const {
    const auto &resolved = variable_->resolve(type_map);
    const auto *time_series = type_cast<TimeSeriesType>(&resolved);
    if (time_series == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"time series variable"},
                                  "time series variable resolved outside its category"};
    }

    return time_series->with_guarantee(guarantee().resolve(type_map));
}

} // namespace bg
