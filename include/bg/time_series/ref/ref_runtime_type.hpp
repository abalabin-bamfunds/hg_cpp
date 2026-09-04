#pragma once

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {

class TimeSeriesReferenceRuntimeType final : public TimeSeriesRuntimeType {
  public:
    TimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type, const TimeSeriesRuntimeType &target) noexcept
        : TimeSeriesRuntimeType{
              type, sizeof(details::ReferenceStorage *), alignof(details::ReferenceStorage *), false, false, true},
          target_{&target} {}
    [[nodiscard]] const TimeSeriesRuntimeType &target_runtime_type() const noexcept { return *target_; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::ReferenceStorage **>(destination), nullptr);
    }

    void invalidate_output(const details::OutputState &output) const override;

    void destroy_value(void *, std::pmr::memory_resource &) const noexcept override {}

  private:
    const TimeSeriesRuntimeType *target_;
};

[[nodiscard]] inline const TimeSeriesRuntimeType &
time_series_reference_runtime_type(const TimeSeriesReferenceType &type, const TimeSeriesRuntimeType &target) {
    struct Key {
        const TimeSeriesReferenceType *type;
        const TimeSeriesRuntimeType *target;

        bool operator==(const Key &) const = default;
    };
    struct Hash {
        [[nodiscard]] std::size_t operator()(const Key &key) const noexcept {
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
