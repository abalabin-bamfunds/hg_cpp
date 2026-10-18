#include <bg/type/time_series_type.hpp>

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

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

bool TimeSeriesType::contains_reference() const { return false; }

const TimeSeriesType &TimeSeriesType::dereferenced_type() const { return *this; }

void TimeSeriesType::append_guarantee(std::string &result) const {
    if (const auto value = guarantee().value(); value && *value != TimeSeriesGuarantee::ordinary) {
        constexpr auto names = std::array<std::string_view, 4>{"", "deduplicated", "final", "const"};
        result += ", ";
        result += names[static_cast<std::size_t>(*value)];
    } else if (const auto *variable = guarantee().variable()) {
        result += ", ";
        result += variable->name().text();
    }
}

const TimeSeriesType &TimeSeriesType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    if (this->guarantee().value() == guarantee) {
        return *this;
    }

    throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"guarantee"},
                              "an unresolved time series type cannot change guarantee"};
}

const TimeSeriesType &TimeSeriesType::with_guarantee_limit(TimeSeriesGuarantee limit) const {
    if (!is_resolved()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"guarantee"},
                                  "an unresolved time series type cannot limit its guarantee"};
    }

    const auto current = guarantee().value();
    const auto limited = weakest(*current, limit);
    if (limited == *current) {
        return *this;
    }

    return with_guarantee(limited);
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

ReferenceAdaptationCounts TimeSeriesType::reference_adaptation_counts_for(const TimeSeriesType &supplied) const
    noexcept {
    const auto *parameter_reference = type_cast<TimeSeriesReferenceType>(this);
    const auto *supplied_reference = type_cast<TimeSeriesReferenceType>(&supplied);
    auto result = ReferenceAdaptationCounts{
        .top_level = static_cast<std::size_t>((parameter_reference == nullptr) != (supplied_reference == nullptr))};

    const auto &parameter_target = parameter_reference == nullptr ? *this : parameter_reference->target_type();
    const auto &supplied_target = supplied_reference == nullptr ? supplied : supplied_reference->target_type();
    const auto contained = parameter_target.contained_reference_adaptation_counts_for(supplied_target);
    result.contained += contained.top_level + contained.contained;
    return result;
}

ReferenceAdaptationCounts
TimeSeriesType::contained_reference_adaptation_counts_for(const TimeSeriesType &supplied) const noexcept {
    const auto *parameter_reference = type_cast<TimeSeriesReferenceType>(this);
    const auto *supplied_reference = type_cast<TimeSeriesReferenceType>(&supplied);
    if (parameter_reference == nullptr && supplied_reference == nullptr) {
        return {};
    }

    auto result = ReferenceAdaptationCounts{
        .contained = static_cast<std::size_t>((parameter_reference == nullptr) != (supplied_reference == nullptr))};

    const auto &parameter_target = parameter_reference == nullptr ? *this : parameter_reference->target_type();
    const auto &supplied_target = supplied_reference == nullptr ? supplied : supplied_reference->target_type();
    const auto nested = parameter_target.contained_reference_adaptation_counts_for(supplied_target);
    result.contained += nested.top_level + nested.contained;
    return result;
}

ReferenceAdaptationCounts FixedItemTimeSeriesType::contained_reference_adaptation_counts_for(
    const TimeSeriesType &supplied) const noexcept {
    const auto *supplied_items = dynamic_cast<const FixedItemTimeSeriesType *>(&supplied);
    if (supplied_items == nullptr || item_count() != supplied_items->item_count()) {
        return {};
    }

    auto result = ReferenceAdaptationCounts{};
    const auto count = item_count() == std::dynamic_extent ? 1 : item_count();
    for (auto index = std::size_t{}; index < count; ++index) {
        const auto item = item_type(index).reference_adaptation_counts_for(supplied_items->item_type(index));
        result.contained += item.top_level + item.contained;
    }
    return result;
}

} // namespace bg
