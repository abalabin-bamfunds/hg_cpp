#include <bg/type/time_series_variable.hpp>

#include <bg/type/type_resolution_error.hpp>

namespace bg {

bool GenericTimeSeriesVariableType::do_match(const Type &supplied, TypeMap &bindings) const {
    const auto *time_series = dynamic_cast<const TimeSeriesType *>(&supplied);
    if (time_series == nullptr || !time_series->is_resolved()) {
        return false;
    }

    const auto &structure = time_series->with_guarantee(TimeSeriesGuarantee::ordinary);
    auto trial = bindings.overlay();
    if (!variable_->match(structure, trial) || !guarantee().match(*time_series->guarantee().value(), trial)) {
        return false;
    }

    bindings.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesVariableType::resolve(const TypeMap &bindings) const {
    const auto &resolved = variable_->resolve(bindings);
    const auto *time_series = dynamic_cast<const TimeSeriesType *>(&resolved);
    if (time_series == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"time series variable"},
                                  "time series variable resolved outside its category"};
    }

    return time_series->with_guarantee(guarantee().resolve(bindings));
}

} // namespace bg
