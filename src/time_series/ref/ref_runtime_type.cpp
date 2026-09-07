#include <bg/time_series/ref/ref_runtime_type.hpp>

#include <bg/time_series/details/output_state.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {

void TimeSeriesReferenceRuntimeType::invalidate_output(const details::OutputState &output) const {
    *static_cast<details::ReferenceAddress *>(output.mutable_data()) = nullptr;
    output.invalidate_value();
}

const TimeSeriesRuntimeType &
time_series_reference_runtime_type(const TimeSeriesReferenceType &type, const TimeSeriesRuntimeType &target) {
    struct Key {
        const TimeSeriesReferenceType *type;
        const TimeSeriesRuntimeType *target;

        bool operator==(const Key &) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key &key) const noexcept {
            const auto type_hash = std::hash<const TimeSeriesReferenceType *>{}(key.type);
            return type_hash ^ (std::hash<const TimeSeriesRuntimeType *>{}(key.target) << 1U);
        }
    };

    static auto mutex = std::mutex{};
    static auto runtime_types = std::unordered_map<Key, std::unique_ptr<TimeSeriesReferenceRuntimeType>, Hash>{};

    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = runtime_types.try_emplace(Key{&type, &target});
    if (inserted) {
        entry->second = std::make_unique<TimeSeriesReferenceRuntimeType>(type, target);
    }
    return *entry->second;
}

} // namespace bg
