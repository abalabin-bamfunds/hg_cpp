#include <bg/time_series/tss/tss_runtime_type.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

std::size_t TimeSeriesSetRuntimeType::set_size(const void *value) const {
    return static_cast<const time_series_set_storage *>(value)->size();
}

bool TimeSeriesSetRuntimeType::set_contains(const void *value, const ScalarData &key) const {
    const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
    return static_cast<const time_series_set_storage *>(value)->contains(key, *key_, layout);
}

item_id_t TimeSeriesSetRuntimeType::find_set_item(const void *value, const ScalarData &key) const {
    const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
    const auto position = static_cast<const time_series_set_storage *>(value)->find_position(key, *key_, layout);
    return position != util::occupancy_index::npos &&
                   static_cast<const time_series_set_storage *>(value)->live().contains(position)
               ? position
               : util::occupancy_index::npos;
}

bool TimeSeriesSetRuntimeType::set_was_added(const TimeSeriesState &last_updated, const void *value,
                                             const ScalarData &key) const {
    const auto &storage = *static_cast<const time_series_set_storage *>(value);
    if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
        return false;
    }
    const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
    const auto position = storage.find_position(key, *key_, layout);
    return position != util::occupancy_index::npos && storage.added().contains(position);
}

bool TimeSeriesSetRuntimeType::set_was_removed(const TimeSeriesState &last_updated, const void *value,
                                               const ScalarData &key) const {
    const auto &storage = *static_cast<const time_series_set_storage *>(value);
    if (storage.change_time() != last_updated.time()) {
        return false;
    }
    const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
    const auto position = storage.find_position(key, *key_, layout);
    return position != util::occupancy_index::npos && storage.removed().contains(position);
}

const void *TimeSeriesSetRuntimeType::set_key(const void *value, item_id_t id) const noexcept {
    const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
    return static_cast<const time_series_set_storage *>(value)->key(id, layout);
}

item_id_t TimeSeriesSetRuntimeType::set_next_item_id(const TimeSeriesState &last_updated, const void *value, Range range,
                                                     item_id_t start) const noexcept {
    const auto &storage = *static_cast<const time_series_set_storage *>(value);
    const auto *positions = range == Range::current ? &storage.live()
                            : range == Range::added  ? &storage.added()
                                                     : &storage.removed();
    if (range != Range::current &&
        (range == Range::added ? (!last_updated.valid() || storage.change_time() != last_updated.time())
                               : storage.change_time() != last_updated.time())) {
        return util::occupancy_index::npos;
    }
    const auto position = positions->first_set(start);
    return position == positions->size() ? util::occupancy_index::npos : position;
}

const TimeSeriesRuntimeType &time_series_set_runtime_type(const TimeSeriesSetType &type,
                                                          const ScalarRuntimeType &key) {
    struct Entry {
        const TimeSeriesSetType *type;
        const ScalarRuntimeType *key;
        std::unique_ptr<TimeSeriesSetRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found =
        std::ranges::find_if(entries, [&](const auto &entry) { return entry.type == &type && entry.key == &key; });
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<TimeSeriesSetRuntimeType>(type, key);
    auto &result = *runtime_type;
    entries.push_back({&type, &key, std::move(runtime_type)});
    return result;
}

} // namespace bg::details
