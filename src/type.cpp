#include <bg/type/type.hpp>

#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>

#include <optional>
#include <utility>

namespace bg {

Type::~Type() = default;

bool Type::accepts(const Type &supplied) const {
    auto type_map = TypeMap{};
    return match(supplied, type_map);
}

bool Type::match(const Type &supplied, TypeMap &type_map) const {
    const auto *parameter_reference = type_cast<TimeSeriesReferenceType>(this);
    const auto *argument_reference = type_cast<TimeSeriesReferenceType>(&supplied);

    if (parameter_reference == nullptr) {
        if (argument_reference == nullptr) {
            return do_match(supplied, type_map);
        }

        if (!argument_reference->is_resolved() || !argument_reference->target_type().is_resolved()) {
            return false;
        }
        return do_match(argument_reference->dereferenced_type(), type_map);
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
