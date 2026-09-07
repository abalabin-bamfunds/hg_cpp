#include <bg/type/time_series_reference_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

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

const TimeSeriesReferenceType &time_series_reference_type(const TimeSeriesType &target_type,
                                                          TimeSeriesGuarantee guarantee) {
    if (dynamic_cast<const TimeSeriesReferenceType *>(&target_type) != nullptr) {
        throw std::invalid_argument{"a time series reference cannot target another reference"};
    }

    static auto mutex = std::mutex{};
    static auto types =
        std::unordered_map<ReferenceTypeKey, std::unique_ptr<ResolvedTimeSeriesReferenceType>, ReferenceTypeKeyHash>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = ReferenceTypeKey{&target_type, guarantee};
    auto [entry, inserted] = types.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<ResolvedTimeSeriesReferenceType>(target_type, guarantee);
    }
    return *entry->second;
}

const TimeSeriesType &TimeSeriesReferenceType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_reference_type(target_type(), guarantee);
}

const Type &GenericTimeSeriesReferenceType::resolve(const TypeMap &bindings) const {
    const auto &resolved_target = target_type().resolve(bindings);
    const auto *target = dynamic_cast<const TimeSeriesType *>(&resolved_target);
    if (target == nullptr || dynamic_cast<const TimeSeriesReferenceType *>(target) != nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"REF"},
                                  "reference target did not resolve to a non-reference time series type"};
    }

    return time_series_reference_type(*target, guarantee().resolve(bindings));
}

} // namespace bg
