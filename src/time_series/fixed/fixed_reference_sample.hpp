#pragma once

#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <memory_resource>

namespace bg::details {

struct SampledFixedCollection {
    SampledFixedCollection(const TimeSeriesRuntimeType &shape, ResolvedTimeSeries previous,
                           ResolvedTimeSeries current, EvaluationTime evaluation_time, allocator &memory);
    ~SampledFixedCollection();

    static SampledFixedCollection *create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                          EvaluationTime evaluation_time, allocator &memory);
    void retain() noexcept { ++reference_count; }
    void release() noexcept;
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    TimeSeriesRuntimeType::ConstItem item(item_id_t id) const;

    const TimeSeriesRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous;
    ResolvedTimeSeries current;
    TimeSeriesState sampled_last_updated;
    allocator memory;
    mutable util::occupancy_index child_samples_positions;
    mutable util::sparse_bunch<SampledReferenceChange> child_samples;
};

} // namespace bg::details
