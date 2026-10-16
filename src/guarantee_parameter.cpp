#include <bg/type/guarantee_parameter.hpp>

namespace bg {

bool GuaranteeParameter::match(TimeSeriesGuarantee supplied, TypeMap &type_map) const {
    if (type_map.matching_structure_only()) {
        return true;
    }

    if (const auto resolved = value()) {
        return satisfies(supplied, *resolved);
    }

    const auto *guarantee_variable = variable();

    return guarantee_variable == nullptr || guarantee_variable->match(supplied, type_map);
}

TimeSeriesGuarantee GuaranteeParameter::resolve(const TypeMap &type_map) const {
    if (const auto resolved = value()) {
        return *resolved;
    }

    const auto *guarantee_variable = variable();
    if (guarantee_variable == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, util::symbol{"guarantee"},
                                  "unspecified guarantee cannot be resolved"};
    }

    return guarantee_variable->resolve(type_map);
}

} // namespace bg
