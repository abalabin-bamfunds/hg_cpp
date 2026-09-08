#include <bg/type/type.hpp>

#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>

#include <optional>
#include <utility>

namespace bg {

Type::~Type() = default;

bool Type::match(const Type &supplied, TypeMap &type_map) const {
    const auto *parameter_reference = dynamic_cast<const TimeSeriesReferenceType *>(this);
    const auto *argument_reference = dynamic_cast<const TimeSeriesReferenceType *>(&supplied);

    if (parameter_reference == nullptr) {
        return do_match(argument_reference == nullptr ? supplied : argument_reference->target_type(), type_map);
    }

    const auto &argument_target = argument_reference == nullptr ? supplied : argument_reference->target_type();
    const auto argument_guarantee = argument_reference == nullptr ? std::optional{TimeSeriesGuarantee::const_}
                                                                  : argument_reference->guarantee().value();
    if (!argument_guarantee) {
        return false;
    }

    auto trial = type_map.overlay();
    if (!parameter_reference->target_type().match(argument_target, trial) ||
        !parameter_reference->guarantee().match(*argument_guarantee, trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

} // namespace bg
