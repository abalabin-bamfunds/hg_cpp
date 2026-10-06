#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/util/index.hpp>

namespace bg::details {

struct SampledSet {
    SampledSet(const TimeSeriesSetRuntimeType &shape, ResolvedTimeSeries previous, ResolvedTimeSeries current,
               EvaluationTime evaluation_time, allocator &memory);
    ~SampledSet();

    static SampledSet *create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                              EvaluationTime evaluation_time, allocator &memory);
    void retain() noexcept { ++reference_count; }
    void release() noexcept;
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    void populate_changes() const;

    const TimeSeriesSetRuntimeType *runtime_type;
    std::size_t reference_count{1};
    TimeSeriesState previous_last_updated;
    TimeSeriesState current_last_updated;
    ResolvedTimeSeries previous;
    ResolvedTimeSeries current;
    TimeSeriesState sampled_last_updated;
    mutable util::index<> added;
    mutable util::index<> removed;
    allocator memory;
    mutable bool changes_populated{};
};

} // namespace bg::details
