#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/util/hash_dictionary.hpp>

#include <concepts>
#include <utility>

namespace bg {

// Stores one operation record for each exact semantic or runtime time series type.
// find(runtime_type) prefers its exact runtime record, then falls back to the semantic record.
// Consumers that select actions independently use find_exact() for each record. Returned
// pointers remain stable until registry destruction.
template <typename Operations> class TimeSeriesDispatch {
  public:
    // Defined only for operation registries whose owning library exports a shared anchor.
    static TimeSeriesDispatch &instance() noexcept;

    TimeSeriesDispatch() = default;
    explicit TimeSeriesDispatch(allocator memory) noexcept : memory_{memory} {}

    ~TimeSeriesDispatch() {
        runtime_operations_.release(memory_.memory_resource());
        type_operations_.release(memory_.memory_resource());
    }

    TimeSeriesDispatch(const TimeSeriesDispatch &) = delete;
    TimeSeriesDispatch &operator=(const TimeSeriesDispatch &) = delete;
    TimeSeriesDispatch(TimeSeriesDispatch &&) = delete;
    TimeSeriesDispatch &operator=(TimeSeriesDispatch &&) = delete;

    template <typename Source>
        requires std::constructible_from<Operations, Source>
    [[nodiscard]] bool add(const TimeSeriesRuntimeType &runtime_type, Source &&operations) {
        return runtime_operations_
            .try_emplace(&runtime_type, memory_.memory_resource(), std::forward<Source>(operations))
            .second;
    }

    template <typename Source>
        requires std::constructible_from<Operations, Source>
    [[nodiscard]] bool add(const TimeSeriesType &type, Source &&operations) {
        return type_operations_.try_emplace(&type, memory_.memory_resource(), std::forward<Source>(operations)).second;
    }

    const Operations *find(const TimeSeriesRuntimeType &runtime_type) const noexcept {
        if (const auto *operations = find_exact(runtime_type)) {
            return operations;
        }
        return find_exact(runtime_type.type());
    }

    const Operations *find_exact(const TimeSeriesRuntimeType &runtime_type) const noexcept {
        return runtime_operations_.find(&runtime_type);
    }

    const Operations *find_exact(const TimeSeriesType &type) const noexcept { return type_operations_.find(&type); }

    const Operations *find(const TimeSeriesType &type) const noexcept { return find_exact(type); }

  private:
    allocator memory_;
    util::hash_dictionary<const TimeSeriesRuntimeType *, Operations> runtime_operations_;
    util::hash_dictionary<const TimeSeriesType *, Operations> type_operations_;
};

} // namespace bg
