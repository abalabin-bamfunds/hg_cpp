#include <bg/type/time_series_list_type.hpp>

#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace bg {
namespace {

class ResolvedTimeSeriesListType final : public TimeSeriesListType {
  public:
    ResolvedTimeSeriesListType(const TimeSeriesType &item_type, std::size_t size, TimeSeriesGuarantee guarantee)
        : TimeSeriesListType(item_type, size, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override { return this == &supplied; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

struct ListTypeKey {
    const TimeSeriesType *item_type;
    std::size_t size;
    TimeSeriesGuarantee guarantee;

    bool operator==(const ListTypeKey &) const = default;
};

struct ListTypeKeyHash {
    [[nodiscard]] std::size_t operator()(const ListTypeKey &key) const noexcept {
        auto result = std::hash<const TimeSeriesType *>{}(key.item_type);
        result ^= std::hash<std::size_t>{}(key.size) + (result << 6) + (result >> 2);
        result ^= std::hash<int>{}(static_cast<int>(key.guarantee)) + (result << 6) + (result >> 2);

        return result;
    }
};

} // namespace

const TimeSeriesType &TimeSeriesListType::item_type(std::size_t index) const {
    if (index >= item_count()) {
        throw std::out_of_range{"time series list item index is out of range"};
    }

    return item_type();
}

bool GenericTimeSeriesListType::match(const Type &supplied, TypeMap &bindings) const {
    const auto *list = dynamic_cast<const TimeSeriesListType *>(&details::unwrap_time_series_reference(supplied));
    if (list == nullptr || !list->is_resolved() || list->item_count() != item_count()) {
        return false;
    }

    auto trial = bindings.overlay();
    if (!item_type().match(list->item_type(), trial) || !guarantee().match(*list->guarantee().value(), trial)) {
        return false;
    }

    bindings.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesListType::resolve(const TypeMap &bindings) const {
    const auto &resolved_item = item_type().resolve(bindings);
    const auto *time_series = dynamic_cast<const TimeSeriesType *>(&resolved_item);
    if (time_series == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"TSL"},
                                  "time series list item did not resolve to a time series type"};
    }

    return time_series_list_type(*time_series, item_count(), guarantee().resolve(bindings));
}

const TimeSeriesListType &time_series_list_type(const TimeSeriesType &item_type, std::size_t size,
                                                TimeSeriesGuarantee guarantee) {
    static auto mutex = std::mutex{};
    static auto types = std::unordered_map<ListTypeKey, std::unique_ptr<ResolvedTimeSeriesListType>, ListTypeKeyHash>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = ListTypeKey{&item_type, size, guarantee};
    auto [entry, inserted] = types.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<ResolvedTimeSeriesListType>(item_type, size, guarantee);
    }

    return *entry->second;
}

} // namespace bg
