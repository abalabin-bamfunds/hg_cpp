#include <bg/type/time_series_set_type.hpp>

#include <bg/type/type_registry.hpp>
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

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

void TimeSeriesSetType::format_name(std::string &result) const {
    result += "TSS<";
    item_type().format_name(result);
    append_guarantee(result);
    result += '>';
}

const TimeSeriesType &TimeSeriesSetType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_set_type(item_type(), guarantee);
}

bool TimeSeriesSetType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *set = type_cast<TimeSeriesSetType>(&supplied);
    if (set == nullptr || !set->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    const auto item_matches = item_type().is_resolved() ? &item_type() == &set->item_type()
                                                        : item_type().match(set->item_type(), trial);
    if (!item_matches || !guarantee().match(*set->guarantee().value(), trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesSetType::resolve(const TypeMap &type_map) const {
    const auto &resolved_item = item_type().resolve(type_map);
    const auto *scalar = type_cast<ScalarType>(&resolved_item);
    if (scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSS"},
                                  "time series set item did not resolve to a scalar type"};
    }
    if (!scalar->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSS"},
                                  "time series set item type does not support hashing"};
    }

    return time_series_set_type(*scalar, guarantee().resolve(type_map));
}

const TimeSeriesSetType &time_series_set_type(const ScalarType &item_type, TimeSeriesGuarantee guarantee) {
    struct Key {
        const ScalarType *item;
        TimeSeriesGuarantee guarantee;

        bool operator==(const Key &) const = default;
    };

    static auto mutex = std::mutex{};
    static auto types = std::vector<std::pair<Key, const ResolvedTimeSeriesSetType *>>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = Key{&item_type, guarantee};
    const auto found = std::ranges::find(types, key, &decltype(types)::value_type::first);
    if (found != types.end()) {
        return *found->second;
    }
    if (!item_type.supports_hash()) {
        throw std::invalid_argument{"time series set item type does not support hashing"};
    }

    types.emplace_back(key, nullptr);
    try {
        const auto &type = TypeRegistry::instance().register_owned(
            std::make_unique<ResolvedTimeSeriesSetType>(item_type, guarantee));
        types.back().second = &type;
        return type;
    } catch (...) {
        types.pop_back();
        throw;
    }
}

} // namespace bg
