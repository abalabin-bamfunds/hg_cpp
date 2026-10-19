#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/util/static_storage.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

void TimeSeriesBundleRuntimeTypeBase::copy_tick(const TimeSeriesInputData &input,
                                                OutputState &output) const {
    if (input.valid()) {
        const auto *source_type = type_cast<TimeSeriesBundleType>(&input.runtime_type().type());
        if (source_type == nullptr || source_type->item_count() < item_count(output.data())) {
            throw std::invalid_argument{"time series bundle copy requires all destination fields"};
        }
    }

    auto source = In<TSB<SCHEMA>>{input};
    auto target = output_factory::make<Out<TSB<SCHEMA>>>(output);
    target.copy_tick(source);
}

void TimeSeriesBundleRuntimeTypeBase::copy(const TimeSeriesInputData &input, OutputState &output) const {
    if (input.valid()) {
        const auto *source_type = type_cast<TimeSeriesBundleType>(&input.runtime_type().type());
        if (source_type == nullptr || source_type->item_count() < item_count(output.data())) {
            throw std::invalid_argument{"time series bundle copy requires all destination fields"};
        }
    }

    auto source = In<TSB<SCHEMA>>{input};
    auto target = output_factory::make<Out<TSB<SCHEMA>>>(output);
    target.copy(source);
}

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
        if (node.input_last_updated(index).valid()) {
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

    static auto mutex = util::static_storage<std::mutex>{};
    static auto entries = util::static_storage<std::vector<Entry>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = entries.get();
    const auto found = std::ranges::find_if(
        cache, [&](const auto &entry) { return entry.type == &type && std::ranges::equal(entry.items, items); });
    if (found != cache.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<RuntimeTimeSeriesBundleRuntimeType>(type, items);
    auto &result = *runtime_type;
    cache.push_back({&type, {items.begin(), items.end()}, std::move(runtime_type)});
    return result;
}

const TimeSeriesRuntimeType &free_time_series_bundle_runtime_type(const TimeSeriesBundleType &type) {
    static auto mutex = util::static_storage<std::mutex>{};
    static auto runtime_types = util::static_storage<std::vector<std::unique_ptr<FreeTimeSeriesBundleRuntimeType>>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = runtime_types.get();
    const auto existing =
        std::ranges::find_if(cache, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != cache.end()) {
        return **existing;
    }
    cache.push_back(std::make_unique<FreeTimeSeriesBundleRuntimeType>(type));
    return *cache.back();
}

} // namespace bg::details
