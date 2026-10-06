#include <bg/time_series/ref/ref_input_sample.hpp>

#include "../tsd/tsd_reference_sample.hpp"
#include "../tss/tss_reference_sample.hpp"

#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
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
                                                       EvaluationTime evaluation_time, allocator &memory) {
    if (!previous.valid() && !current.valid()) {
        return {};
    }

    const auto *runtime_type = current.runtime_type != nullptr ? current.runtime_type : previous.runtime_type;
    if (runtime_type == nullptr) {
        return {};
    }

    if (type_cast<TimeSeriesSetType>(&runtime_type->type()) != nullptr) {
        return SampledReferenceChange{SampledSet::create(previous, current, evaluation_time, memory)};
    }

    if (type_cast<TimeSeriesDictionaryType>(&runtime_type->type()) != nullptr) {
        return SampledReferenceChange{SampledDictionary::create(previous, current, evaluation_time, memory)};
    }

    return {};
}

void SampledReferenceChange::retain() const noexcept {
    if (!*this) {
        return;
    }
    visit([](SampledSet *sample) { sample->retain(); },
          [](SampledDictionary *sample) { sample->retain(); });
}

void SampledReferenceChange::release() const noexcept {
    if (!*this) {
        return;
    }
    visit([](SampledSet *sample) { sample->release(); },
          [](SampledDictionary *sample) { sample->release(); });
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
    const auto previous_last_updated =
        previous.last_updated == nullptr ? initial_time_series_state : *previous.last_updated;
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

    auto sample = SampledReferenceChange::create(previous, current, evaluation_time, *memory_);
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
