#include <bg/time_series/ref/ref_input_sample.hpp>

#include "../fixed/fixed_reference_sample.hpp"
#include "../tsd/tsd_reference_sample.hpp"
#include "../tss/tss_reference_sample.hpp"

#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>

namespace bg::details {

TimeSeriesRuntimeType::ConstItem ReferenceInputSampleStorage::input() const noexcept {
    assert(sample());
    return sample().input();
}

ResolvedTimeSeries ReferenceInputSampleStorage::previous() const noexcept {
    assert(sample());
    return sample().previous();
}

SampledReferenceChange SampledReferenceChange::create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                                       const TimeSeriesType &type,
                                                       EvaluationTime evaluation_time, allocator &memory) {
    if (previous.runtime_type == nullptr && current.runtime_type == nullptr) {
        return {};
    }

    const auto *list_type = type_cast<TimeSeriesListType>(&type);
    if ((list_type != nullptr && list_type->item_count() != std::dynamic_extent) ||
        type_cast<TimeSeriesBundleType>(&type) != nullptr) {
        return SampledReferenceChange{SampledFixedCollection::create(type, previous, current, evaluation_time, memory)};
    }

    if (!previous.valid() && !current.valid()) {
        return {};
    }

    if (const auto *set_type = type_cast<TimeSeriesSetType>(&type); set_type != nullptr) {
        return SampledReferenceChange{SampledSet::create(*set_type, previous, current, evaluation_time, memory)};
    }

    if (const auto *dictionary_type = type_cast<TimeSeriesDictionaryType>(&type); dictionary_type != nullptr) {
        return SampledReferenceChange{
            SampledDictionary::create(*dictionary_type, previous, current, evaluation_time, memory)};
    }

    return {};
}

void SampledReferenceChange::retain() const noexcept {
    if (!*this) {
        return;
    }
    visit([](SampledSet *sample) { sample->retain(); },
          [](SampledDictionary *sample) { sample->retain(); },
          [](SampledFixedCollection *sample) { sample->retain(); });
}

void SampledReferenceChange::release() const noexcept {
    if (!*this) {
        return;
    }
    visit([](SampledSet *sample) { sample->release(); },
          [](SampledDictionary *sample) { sample->release(); },
          [](SampledFixedCollection *sample) { sample->release(); });
}

TimeSeriesRuntimeType::ConstItem SampledReferenceChange::input() const noexcept {
    assert(*this);
    return visit([](const auto *sample) { return sample->input(); });
}

ResolvedTimeSeries SampledReferenceChange::previous() const noexcept {
    assert(*this);
    return visit([](const auto *sample) { return sample->previous; });
}

ReferenceInputSampleLookup::~ReferenceInputSampleLookup() { clear(); }

SampledReferenceChange ReferenceInputSampleLookup::get(Reference previous_reference,
                                                        Reference current_reference, ResolvedTimeSeries previous,
                                                        ResolvedTimeSeries current,
                                                        const TimeSeriesRuntimeType &runtime_type,
                                                        EvaluationTime evaluation_time) {
    const auto previous_last_updated = previous.last_updated;
    const auto key = Entry{previous_reference,
                           current_reference,
                           &runtime_type,
                           previous_last_updated,
                           previous.storage,
                           previous.runtime_type,
                           {}};
    if (const auto *existing = entries_.find(key)) {
        return existing->sample;
    }

    auto sample = SampledReferenceChange::create(previous, current, runtime_type.type(), evaluation_time, *memory_);
    if (!sample) {
        return {};
    }
    auto release = util::scope_exit{[&]() noexcept { sample.release(); }};
    const auto [entry, inserted] = entries_.insert(
        Entry{previous_reference, current_reference, &runtime_type, previous_last_updated, previous.storage,
              previous.runtime_type, sample},
        memory_->memory_resource());
    assert(inserted);
    static_cast<void>(entry);
    release.release();
    return sample;
}

void ReferenceInputSampleLookup::clear() noexcept {
    for (const auto &entry : entries_) {
        entry.sample.release();
    }
    entries_.release(memory_->memory_resource());
}

} // namespace bg::details
