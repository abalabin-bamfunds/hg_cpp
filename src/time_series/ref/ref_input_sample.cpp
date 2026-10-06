#include <bg/time_series/ref/ref_input_sample.hpp>

#include "../tsd/tsd_reference_sample.hpp"
#include "../tss/tss_reference_sample.hpp"

#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/graph/runtime_graph.hpp>

#include <cassert>
#include <concepts>
#include <type_traits>

namespace bg::details {

TimeSeriesRuntimeType::ConstItem ReferenceInputSampleStorage::input() const noexcept {
    assert(sample());
    return sample().visit([](const auto *sample) { return reference_sample_item(*sample); });
}

ResolvedTimeSeries ReferenceInputSampleStorage::previous() const noexcept {
    assert(sample());
    return sample().visit([](const auto *sample) { return reference_sample_previous(*sample); });
}

SampledReferenceChange make_reference_sample(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                             EvaluationTime evaluation_time, allocator &memory) {
    if (!previous.valid() && !current.valid()) {
        return {};
    }

    const auto *runtime_type = current.runtime_type != nullptr ? current.runtime_type : previous.runtime_type;
    if (runtime_type == nullptr) {
        return {};
    }

    if (type_cast<TimeSeriesSetType>(&runtime_type->type()) != nullptr) {
        auto *sample = make_set_sample(previous, current, evaluation_time, memory);
        return SampledReferenceChange{sample};
    }

    if (type_cast<TimeSeriesDictionaryType>(&runtime_type->type()) != nullptr) {
        auto *sample = make_dictionary_sample(previous, current, evaluation_time, memory);
        if (sample == nullptr) {
            return {};
        }
        return SampledReferenceChange{sample};
    }

    return {};
}

void destroy_reference_sample(SampledReferenceChange sample) noexcept {
    if (!sample) {
        return;
    }
    sample.visit([](auto *value) {
        using T = std::remove_pointer_t<decltype(value)>;
        if constexpr (std::same_as<T, SampledSet>) {
            destroy_set_sample(value);
        } else {
            destroy_dictionary_sample(value);
        }
    });
}

TimeSeriesRuntimeType::ConstItem reference_sample_item(const SampledSet &sample) noexcept {
    return set_sample_item(sample);
}

TimeSeriesRuntimeType::ConstItem reference_sample_item(const SampledDictionary &sample) noexcept {
    return dictionary_sample_item(sample);
}

ResolvedTimeSeries reference_sample_previous(const SampledSet &sample) noexcept { return set_sample_previous(sample); }

ResolvedTimeSeries reference_sample_previous(const SampledDictionary &sample) noexcept {
    return dictionary_sample_previous(sample);
}

ReferenceInputSampleCache::~ReferenceInputSampleCache() { clear(); }

SampledReferenceChange ReferenceInputSampleCache::get(const RuntimeGraph &publisher, const void *output,
                                                     Reference previous_reference,
                                                     Reference current_reference, ResolvedTimeSeries previous,
                                                     ResolvedTimeSeries current,
                                                     const TimeSeriesRuntimeType &runtime_type,
                                                     EvaluationTime evaluation_time) {
    const auto key = Entry{&publisher, output, previous_reference, current_reference, &runtime_type, {}};
    if (const auto *existing = entries_.find(key)) {
        return existing->sample;
    }

    auto sample_memory = publisher.allocation();
    auto sample = make_reference_sample(previous, current, evaluation_time, sample_memory);
    if (!sample) {
        return {};
    }
    auto destroy = util::scope_exit{[&]() noexcept { destroy_reference_sample(sample); }};
    const auto [entry, inserted] = entries_.insert(
        Entry{&publisher, output, previous_reference, current_reference, &runtime_type, sample},
        memory_->memory_resource());
    assert(inserted);
    static_cast<void>(entry);
    destroy.release();
    return sample;
}

void ReferenceInputSampleCache::clear() noexcept {
    for (const auto &entry : entries_) {
        destroy_reference_sample(entry.sample);
    }
    entries_.release(memory_->memory_resource());
}

} // namespace bg::details
