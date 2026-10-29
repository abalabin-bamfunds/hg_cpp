#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/util/index.hpp>

#include <memory_resource>
#include <vector>

namespace bg::details {

struct SampledSet {
    struct ItemIdRemapping {
        item_id_t old_id;
        item_id_t new_id;
    };

    SampledSet(const TimeSeriesSetRuntimeType &shape, ResolvedTimeSeries previous, ResolvedTimeSeries current,
               EvaluationTime evaluation_time, allocator &memory);
    ~SampledSet();

    static SampledSet *create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                              EvaluationTime evaluation_time, allocator &memory);
    void retain() noexcept { ++reference_count; }
    void release() noexcept;
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    bool previous_contains(const ScalarData &key) const;
    void populate_changes() const;

    const TimeSeriesSetRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous;
    ResolvedTimeSeries current;
    TimeSeriesState sampled_last_updated;
    mutable util::index<> added;
    mutable util::index<> removed;
    mutable std::pmr::vector<ItemIdRemapping> item_id_remappings;
    allocator memory;
    mutable bool changes_populated{};
};

} // namespace bg::details
