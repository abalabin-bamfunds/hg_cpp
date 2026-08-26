#include <bg/type/guarantee_parameter.hpp>

namespace bg {

bool GuaranteeParameter::match(TimeSeriesGuarantee supplied, TypeMap &bindings) const {
    if (const auto resolved = value()) {
        return satisfies(supplied, *resolved);
    }

    const auto *guarantee_variable = variable();

    return guarantee_variable == nullptr || guarantee_variable->match(supplied, bindings);
}

TimeSeriesGuarantee GuaranteeParameter::resolve(const TypeMap &bindings) const {
    if (const auto resolved = value()) {
        return *resolved;
    }

    const auto *guarantee_variable = variable();
    if (guarantee_variable == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::unbound_variable,
                                  util::symbol{"guarantee"},
                                  "unspecified guarantee cannot be resolved"};
    }

    return guarantee_variable->resolve(bindings);
}

} // namespace bg
