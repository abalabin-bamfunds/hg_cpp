#include <bg/type/time_series_set_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace bg {
namespace {

class ResolvedTimeSeriesSetType final : public TimeSeriesSetType {
  public:
    ResolvedTimeSeriesSetType(const ScalarType &item_type, TimeSeriesGuarantee guarantee)
        : TimeSeriesSetType(item_type, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

const TimeSeriesType &TimeSeriesSetType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_set_type(item_type(), guarantee);
}

bool GenericTimeSeriesSetType::do_match(const Type &supplied, TypeMap &bindings) const {
    const auto *set = dynamic_cast<const TimeSeriesSetType *>(&supplied);
    if (set == nullptr || !set->is_resolved()) {
        return false;
    }

    auto trial = bindings.overlay();
    if (!item_type().match(set->item_type(), trial) || !guarantee().match(*set->guarantee().value(), trial)) {
        return false;
    }

    bindings.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesSetType::resolve(const TypeMap &bindings) const {
    const auto &resolved_item = item_type().resolve(bindings);
    const auto *scalar = dynamic_cast<const ScalarType *>(&resolved_item);
    if (scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"TSS"},
                                  "time series set item did not resolve to a scalar type"};
    }
    if (!scalar->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"TSS"},
                                  "time series set item type does not support hashing"};
    }

    return time_series_set_type(*scalar, guarantee().resolve(bindings));
}

const TimeSeriesSetType &time_series_set_type(const ScalarType &item_type, TimeSeriesGuarantee guarantee) {
    struct Key {
        const ScalarType *item;
        TimeSeriesGuarantee guarantee;

        bool operator==(const Key &) const = default;
    };

    static auto mutex = std::mutex{};
    static auto types = std::vector<std::pair<Key, std::unique_ptr<ResolvedTimeSeriesSetType>>>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = Key{&item_type, guarantee};
    const auto found = std::ranges::find(types, key, &decltype(types)::value_type::first);
    if (found != types.end()) {
        return *found->second;
    }
    if (!item_type.supports_hash()) {
        throw std::invalid_argument{"time series set item type does not support hashing"};
    }

    types.emplace_back(key, std::make_unique<ResolvedTimeSeriesSetType>(item_type, guarantee));
    return *types.back().second;
}

} // namespace bg
