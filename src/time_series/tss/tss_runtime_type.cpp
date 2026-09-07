#include <bg/time_series/tss/tss_runtime_type.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

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
