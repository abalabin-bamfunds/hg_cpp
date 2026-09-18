#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

Node free_bundle_node(const free_time_series_bundle_storage &storage) noexcept {
    return Node::from_output(*storage.node, const_cast<free_time_series_bundle_storage *>(&storage));
}

bool free_bundle_valid(const free_time_series_bundle_storage &storage) noexcept {
    if (storage.node == nullptr) {
        return false;
    }
    const auto node = free_bundle_node(storage);
    if (node.input_count() == 0) {
        return true;
    }

    for (auto index = std::size_t{}; index < node.input_count(); ++index) {
        if (node.input_last_updated(index) > MIN_DT) {
            return true;
        }
    }
    return false;
}

const TimeSeriesRuntimeType &
time_series_bundle_runtime_type(const TimeSeriesBundleType &type,
                                std::span<const TimeSeriesRuntimeType *const> items) {
    struct Entry {
        const TimeSeriesBundleType *type;
        std::vector<const TimeSeriesRuntimeType *> items;
        std::unique_ptr<RuntimeTimeSeriesBundleRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(
        entries, [&](const auto &entry) { return entry.type == &type && std::ranges::equal(entry.items, items); });
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<RuntimeTimeSeriesBundleRuntimeType>(type, items);
    auto &result = *runtime_type;
    entries.push_back({&type, {items.begin(), items.end()}, std::move(runtime_type)});
    return result;
}

const TimeSeriesRuntimeType &free_time_series_bundle_runtime_type(const TimeSeriesBundleType &type) {
    static auto mutex = std::mutex{};
    static auto runtime_types = std::vector<std::unique_ptr<FreeTimeSeriesBundleRuntimeType>>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing =
        std::ranges::find_if(runtime_types, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != runtime_types.end()) {
        return **existing;
    }
    runtime_types.push_back(std::make_unique<FreeTimeSeriesBundleRuntimeType>(type));
    return *runtime_types.back();
}

} // namespace bg::details
