#include <bg/type/time_series_list_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <algorithm>
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

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

struct ListTypeKey {
    const TimeSeriesType *item_type;
    std::size_t size;
    TimeSeriesGuarantee guarantee;

    bool operator==(const ListTypeKey &) const = default;
};

struct ListTypeKeyHash {
    std::size_t operator()(const ListTypeKey &key) const noexcept {
        auto result = std::hash<const TimeSeriesType *>{}(key.item_type);
        result ^= std::hash<std::size_t>{}(key.size) + (result << 6) + (result >> 2);
        result ^= std::hash<int>{}(static_cast<int>(key.guarantee)) + (result << 6) + (result >> 2);

        return result;
    }
};

} // namespace

const TimeSeriesListType &TimeSeriesListType::from_items(std::span<const TimeSeriesType *const> items,
                                                        TimeSeriesGuarantee collection_guarantee) {
    if (items.empty()) {
        throw std::invalid_argument{"a time series list type requires at least one item"};
    }
    if (std::ranges::any_of(items, [](const auto *item) { return item == nullptr || !item->is_resolved(); })) {
        throw std::invalid_argument{"a time series list type requires resolved item types"};
    }

    const auto &first = items.front()->dereferenced_type();
    auto guarantee = *first.guarantee().value();
    const auto &shape = first.with_guarantee(TimeSeriesGuarantee::ordinary);

    for (const auto *item : items.subspan(1)) {
        const auto &candidate = item->dereferenced_type();
        if (&candidate.with_guarantee(TimeSeriesGuarantee::ordinary) != &shape) {
            throw std::invalid_argument{"a time series list requires compatible item types"};
        }
        guarantee = weakest(guarantee, *candidate.guarantee().value());
    }

    return time_series_list_type(first.with_guarantee(guarantee), items.size(), collection_guarantee);
}

const TimeSeriesType &TimeSeriesListType::item_type(std::size_t index) const {
    if (index >= item_count()) {
        throw std::out_of_range{"time series list item index is out of range"};
    }

    return item_type();
}

const TimeSeriesType &TimeSeriesListType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    if (const auto resolved_size = size().value()) {
        return time_series_list_type(item_type(), *resolved_size, guarantee);
    }
    return TimeSeriesType::with_guarantee(guarantee);
}

const TimeSeriesType &TimeSeriesListType::with_guarantee_limit(TimeSeriesGuarantee limit) const {
    if (!is_resolved()) {
        return TimeSeriesType::with_guarantee_limit(limit);
    }

    const auto current = *guarantee().value();
    const auto &limited_item = item_type().with_guarantee_limit(limit);
    const auto limited_guarantee = weakest(current, limit);
    if (&limited_item == &item_type() && limited_guarantee == current) {
        return *this;
    }

    return time_series_list_type(limited_item, item_count(), limited_guarantee);
}

bool TimeSeriesListType::do_match(const Type &supplied, TypeMap &type_map) const {
    return do_match(supplied, type_map, std::nullopt);
}

bool TimeSeriesListType::do_match(const Type &supplied, TypeMap &type_map,
                                  std::optional<TimeSeriesGuarantee> reference_limit) const {
    const auto *list = type_cast<TimeSeriesListType>(&supplied);
    if (list == nullptr || !list->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    const auto supplied_guarantee = *list->guarantee().value();
    const auto observed_guarantee = reference_limit ? weakest(supplied_guarantee, *reference_limit)
                                                    : supplied_guarantee;
    if (!size().match(list->item_count(), trial) || !item_type().match(list->item_type(), trial, reference_limit) ||
        !guarantee().match(observed_guarantee, trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesListType::resolve(const TypeMap &type_map) const {
    const auto &resolved_item = item_type().resolve(type_map);
    const auto *time_series = type_cast<TimeSeriesType>(&resolved_item);
    if (time_series == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSL"},
                                  "time series list item did not resolve to a time series type"};
    }

    return time_series_list_type(*time_series, size().resolve(type_map), guarantee().resolve(type_map));
}

const TimeSeriesListType &time_series_list_type(const TimeSeriesType &item_type, std::size_t size,
                                                TimeSeriesGuarantee guarantee) {
    static auto mutex = std::mutex{};
    static auto types = std::unordered_map<ListTypeKey, const ResolvedTimeSeriesListType *, ListTypeKeyHash>{};

    const auto &effective_item = item_type.with_guarantee(strongest(*item_type.guarantee().value(), guarantee));
    const auto lock = std::lock_guard{mutex};
    const auto key = ListTypeKey{&effective_item, size, guarantee};
    auto [entry, inserted] = types.try_emplace(key);
    if (inserted) {
        try {
            entry->second = &TypeRegistry::instance().register_owned(
                std::make_unique<ResolvedTimeSeriesListType>(effective_item, size, guarantee));
        } catch (...) {
            types.erase(entry);
            throw;
        }
    }

    return *entry->second;
}

} // namespace bg
