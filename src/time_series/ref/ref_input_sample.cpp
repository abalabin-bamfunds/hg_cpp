#include <bg/time_series/ref/ref_input_sample.hpp>

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

SampledReferenceChange SampledReferenceChange::create(const TimeSeriesRuntimeType &requested_runtime_type,
                                                       ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                                       EvaluationTime evaluation_time, allocator &memory) {
    const auto factory = requested_runtime_type.reference_sample_factory();
    if (factory == nullptr) {
        return {};
    }

    return SampledReferenceChange{factory(requested_runtime_type.type(), previous, current, evaluation_time, memory)};
}

void SampledReferenceChange::retain() const noexcept {
    if (sample_ != nullptr) {
        sample_->retain();
    }
}

void SampledReferenceChange::release() const noexcept {
    if (sample_ != nullptr) {
        sample_->release();
    }
}

TimeSeriesRuntimeType::ConstItem SampledReferenceChange::input() const noexcept {
    assert(sample_ != nullptr);
    return sample_->input();
}

ResolvedTimeSeries SampledReferenceChange::previous() const noexcept {
    assert(sample_ != nullptr);
    return sample_->previous();
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

    auto sample = SampledReferenceChange::create(runtime_type, previous, current, evaluation_time, *memory_);
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
