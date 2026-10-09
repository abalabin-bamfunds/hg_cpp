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
    return match(supplied, type_map, std::nullopt);
}

bool Type::match(const Type &supplied, TypeMap &type_map,
                 std::optional<TimeSeriesGuarantee> reference_limit) const {
    const auto *parameter_reference = type_cast<TimeSeriesReferenceType>(this);
    const auto *argument_reference = type_cast<TimeSeriesReferenceType>(&supplied);

    if (parameter_reference != nullptr) {
        const auto &argument_target = argument_reference == nullptr ? supplied : argument_reference->target_type();
        const auto argument_guarantee = argument_reference == nullptr ? TimeSeriesGuarantee::const_
                                                                      : argument_reference->guarantee().value();
        if (!argument_guarantee) {
            return false;
        }

        const auto observed_guarantee = reference_limit
                                            ? weakest(*argument_guarantee, *reference_limit)
                                            : *argument_guarantee;
        auto trial = type_map.overlay();
        if (!parameter_reference->target_type().match(argument_target, trial) ||
            !parameter_reference->guarantee().match(observed_guarantee, trial)) {
            return false;
        }

        type_map.apply(std::move(trial));
        return true;
    }

    auto effective_limit = reference_limit;
    auto const *argument = &supplied;
    if (argument_reference != nullptr) {
        if (!argument_reference->is_resolved() || !argument_reference->target_type().is_resolved()) {
            return false;
        }

        const auto argument_limit = argument_reference->reference_limit();
        effective_limit = effective_limit ? weakest(*effective_limit, argument_limit) : argument_limit;
        argument = &argument_reference->target_type();
    }

    if (!effective_limit) {
        return do_match(*argument, type_map);
    }

    return do_match(*argument, type_map, effective_limit);
}

bool Type::do_match(const Type &supplied, TypeMap &type_map,
                    std::optional<TimeSeriesGuarantee> reference_limit) const {
    const auto *time_series = type_cast<TimeSeriesType>(&supplied);
    if (reference_limit && time_series != nullptr && time_series->is_resolved()) {
        return do_match(time_series->with_guarantee_limit(*reference_limit), type_map);
    }

    return do_match(supplied, type_map);
}

} // namespace bg
