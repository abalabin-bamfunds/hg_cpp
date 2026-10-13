#include <bg/type/time_series_reference_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <stdexcept>
#include <memory>
#include <utility>

namespace bg {
namespace {

class ResolvedTimeSeriesReferenceType final : public TimeSeriesReferenceType {
  public:
    ResolvedTimeSeriesReferenceType(const TimeSeriesType &target_type, TimeSeriesGuarantee guarantee)
        : TimeSeriesReferenceType(target_type, guarantee) {}

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

struct ReferenceTypeKey {
    const TimeSeriesType *target_type;
    TimeSeriesGuarantee guarantee;

    bool operator==(const ReferenceTypeKey &) const = default;
};

struct ReferenceTypeKeyHash {
    std::size_t operator()(const ReferenceTypeKey &key) const noexcept {
        const auto target = std::hash<const TimeSeriesType *>{}(key.target_type);
        const auto guarantee = std::hash<int>{}(static_cast<int>(key.guarantee));
        return target ^ (guarantee << 1U);
    }
};

} // namespace

void TimeSeriesReferenceType::format_name(std::string &result) const {
    result += "REF<";
    target_type().format_name(result);
    append_guarantee(result);
    result += '>';
}

const TimeSeriesReferenceType &time_series_reference_type(const TimeSeriesType &target_type,
                                                          TimeSeriesGuarantee guarantee) {
    if (type_cast<TimeSeriesReferenceType>(&target_type) != nullptr) {
        throw std::invalid_argument{"a time series reference cannot target another reference"};
    }

    return TypeRegistry::instance().get_or_create<ResolvedTimeSeriesReferenceType, ReferenceTypeKeyHash>(
        ReferenceTypeKey{&target_type, guarantee},
        [&] { return std::make_unique<ResolvedTimeSeriesReferenceType>(target_type, guarantee); });
}

const TimeSeriesReferenceType &details::inherited_reference_transport_type(
    const TimeSeriesReferenceType &target_type) {
    return TypeRegistry::instance().get_or_create<ResolvedTimeSeriesReferenceType, ReferenceTypeKeyHash>(
        ReferenceTypeKey{&target_type, TimeSeriesGuarantee::const_},
        [&] { return std::make_unique<ResolvedTimeSeriesReferenceType>(target_type, TimeSeriesGuarantee::const_); });
}

const TimeSeriesType &TimeSeriesReferenceType::dereferenced_type() const {
    if (!is_resolved() || !target_type().is_resolved() || !guarantee().value()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"REF"},
                                  "an unresolved reference cannot be dereferenced"};
    }

    return target_type().with_guarantee_limit(reference_limit());
}

TimeSeriesGuarantee TimeSeriesReferenceType::reference_limit() const {
    const auto reference_guarantee = guarantee().value();
    if (!reference_guarantee) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"REF"},
                                  "an unresolved reference has no guarantee limit"};
    }

    if (*reference_guarantee == TimeSeriesGuarantee::final || *reference_guarantee == TimeSeriesGuarantee::const_) {
        return *reference_guarantee;
    }

    return TimeSeriesGuarantee::ordinary;
}

const GenericTimeSeriesReferenceType &generic_time_series_reference_type(
    const TimeSeriesType &target_type, TimeSeriesGuarantee guarantee) {
    return TypeRegistry::instance().get_or_create<GenericTimeSeriesReferenceType, ReferenceTypeKeyHash>(
        ReferenceTypeKey{&target_type, guarantee},
        [&] { return std::make_unique<GenericTimeSeriesReferenceType>(target_type, guarantee); });
}

const TimeSeriesType &TimeSeriesReferenceType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_reference_type(target_type(), guarantee);
}

const Type &GenericTimeSeriesReferenceType::resolve(const TypeMap &type_map) const {
    const auto &resolved_target = target_type().resolve(type_map);
    const auto *target = type_cast<TimeSeriesType>(&resolved_target);
    if (target == nullptr || type_cast<TimeSeriesReferenceType>(target) != nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"REF"},
                                  "reference target did not resolve to a non-reference time series type"};
    }

    return time_series_reference_type(*target, guarantee().resolve(type_map));
}

} // namespace bg
