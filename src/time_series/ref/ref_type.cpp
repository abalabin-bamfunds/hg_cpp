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

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override { return this == &supplied; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

struct ReferenceTypeKey {
    const TimeSeriesType *target_type;
    TimeSeriesGuarantee guarantee;

    bool operator==(const ReferenceTypeKey &) const = default;
};

struct ReferenceTypeKeyHash {
    [[nodiscard]] std::size_t operator()(const ReferenceTypeKey &key) const noexcept {
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

bool GenericTimeSeriesReferenceType::match(const Type &supplied, TypeMap &bindings) const {
    const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&supplied);
    const auto *target =
        reference == nullptr ? dynamic_cast<const TimeSeriesType *>(&supplied) : &reference->target_type();
    if (target == nullptr || !target->is_resolved()) {
        return false;
    }

    auto trial = bindings.overlay();
    const auto supplied_guarantee =
        reference == nullptr ? TimeSeriesGuarantee::const_ : *reference->guarantee().value();
    if (!target_type().match(*target, trial) || !guarantee().match(supplied_guarantee, trial)) {
        return false;
    }

    bindings.apply(std::move(trial));
    return true;
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
