#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/index.hpp>

#include <memory_resource>
#include <vector>

namespace bg::details {

struct SampledSet;

struct SampledDictionary final : ReferenceSample {
    enum class Target { previous, current };

    struct Child {
        item_id_t id;
        SampledReferenceChange sample;
    };

    struct ItemIdRemapping {
        item_id_t old_id;
        item_id_t new_id;
    };

    SampledDictionary(const TimeSeriesDictionaryType &type, ResolvedTimeSeries previous,
                      ResolvedTimeSeries current, EvaluationTime evaluation_time, allocator &memory);
    ~SampledDictionary() override;

    static SampledDictionary *create(const TimeSeriesDictionaryType &type, ResolvedTimeSeries previous,
                                     ResolvedTimeSeries current, EvaluationTime evaluation_time,
                                     allocator &memory);
    ResolvedTimeSeries key_set(Target target) const;
    void retain() noexcept override { ++reference_count; }
    void release() noexcept override;
    TimeSeriesRuntimeType::ConstItem input() const noexcept override;
    ResolvedTimeSeries previous() const noexcept override { return previous_source; }
    void populate_changes() const;

    const TimeSeriesDictionaryRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous_source;
    ResolvedTimeSeries current_source;
    TimeSeriesState sampled_last_updated;
    mutable util::index<> added;
    mutable util::index<> removed;
    mutable util::index<> updated;
    mutable std::pmr::vector<Child> children;
    mutable std::pmr::vector<ItemIdRemapping> item_id_remappings;
    mutable SampledSet *key_set_sample{};
    allocator memory;
    mutable bool changes_populated{};

    void append_child(item_id_t current_id, item_id_t previous_id,
                      const TimeSeriesRuntimeType::ConstItem *previous_item,
                      const TimeSeriesRuntimeType::ConstItem *current_item) const;
};

} // namespace bg::details
