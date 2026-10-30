#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/util/index.hpp>

#include <memory_resource>
#include <vector>

namespace bg::details {

struct SampledSet final : ReferenceSample {
    struct ItemIdRemapping {
        item_id_t old_id;
        item_id_t new_id;
    };

    SampledSet(const TimeSeriesSetType &type, ResolvedTimeSeries previous, ResolvedTimeSeries current,
               EvaluationTime evaluation_time, allocator &memory);
    ~SampledSet() override;

    static SampledSet *create(const TimeSeriesSetType &type, ResolvedTimeSeries previous,
                              ResolvedTimeSeries current, EvaluationTime evaluation_time, allocator &memory);
    void retain() noexcept override { ++reference_count; }
    void release() noexcept override;
    TimeSeriesRuntimeType::ConstItem input() const noexcept override;
    ResolvedTimeSeries previous() const noexcept override { return previous_source; }
    bool previous_contains(const ScalarData &key) const;
    void populate_changes() const;

    const TimeSeriesSetRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous_source;
    ResolvedTimeSeries current_source;
    TimeSeriesState sampled_last_updated;
    mutable util::index<> added;
    mutable util::index<> removed;
    mutable std::pmr::vector<ItemIdRemapping> item_id_remappings;
    allocator memory;
    mutable bool changes_populated{};
};

} // namespace bg::details
