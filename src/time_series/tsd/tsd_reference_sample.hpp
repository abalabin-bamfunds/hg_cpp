#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/index.hpp>

#include <memory_resource>
#include <vector>

namespace bg::details {

struct SampledDictionary {
    enum class Target { previous, current };

    struct Child {
        item_id_t id;
        SampledReferenceChange sample;
    };

    struct ItemIdRemapping {
        item_id_t old_id;
        item_id_t new_id;
    };

    SampledDictionary(const TimeSeriesDictionaryRuntimeType &shape, ResolvedTimeSeries previous,
                      ResolvedTimeSeries current, EvaluationTime evaluation_time, allocator &memory);
    ~SampledDictionary();

    static SampledDictionary *create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                      EvaluationTime evaluation_time, allocator &memory);
    ResolvedTimeSeries key_set(Target target) const;
    void retain() noexcept { ++reference_count; }
    void release() noexcept;
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    void populate_changes() const;

    const TimeSeriesDictionaryRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous;
    ResolvedTimeSeries current;
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
