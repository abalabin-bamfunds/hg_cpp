#include <bg/time_series/tsl/tsl_runtime_type.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

const TimeSeriesRuntimeType &
fixed_time_series_list_runtime_type(const TimeSeriesListType &type, const TimeSeriesRuntimeType &item) {
    struct Entry {
        const TimeSeriesListType *type;
        const TimeSeriesRuntimeType *item;
        std::unique_ptr<FixedTimeSeriesListRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found =
        std::ranges::find_if(entries, [&](const auto &entry) { return entry.type == &type && entry.item == &item; });
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<FixedTimeSeriesListRuntimeType>(type, item);
    auto &result = *runtime_type;
    entries.push_back({&type, &item, std::move(runtime_type)});
    return result;
}

} // namespace bg::details
