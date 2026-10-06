#pragma once

#include <bg/allocator.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/util/adaptive_set.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory_resource>

namespace bg::details {

struct SampledSet;
struct SampledDictionary;

struct SampledReferenceChange
    : util::variant_ptr_with_alignment<alignof(void *), SampledSet, SampledDictionary> {
    using Base = util::variant_ptr_with_alignment<alignof(void *), SampledSet, SampledDictionary>;
    using Base::Base;

    static SampledReferenceChange create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                         EvaluationTime evaluation_time, allocator &memory);
    void retain() const noexcept;
    void release() const noexcept;
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    ResolvedTimeSeries previous() const noexcept;
};

static_assert(sizeof(SampledReferenceChange) == sizeof(void *));

class ReferenceInputSampleLookup {
  public:
    explicit ReferenceInputSampleLookup(allocator &memory) : memory_{&memory} {}

    ~ReferenceInputSampleLookup();

    SampledReferenceChange get(Reference previous_reference, Reference current_reference,
                               ResolvedTimeSeries previous, ResolvedTimeSeries current,
                               const TimeSeriesRuntimeType &runtime_type, EvaluationTime evaluation_time);
    void clear() noexcept;

  private:
    struct Entry {
        Reference previous_reference;
        Reference current_reference;
        const TimeSeriesRuntimeType *runtime_type;
        TimeSeriesState previous_last_updated;
        const void *previous_storage;
        const TimeSeriesRuntimeType *previous_runtime_type;
        SampledReferenceChange sample;
    };

    struct EntryHash {
        std::size_t operator()(const Entry &entry) const noexcept {
            const auto combine = [](std::size_t seed, std::size_t value) {
                return seed ^ (value + std::size_t{0x9e3779b9} + (seed << 6) + (seed >> 2));
            };
            auto result = std::hash<std::uintptr_t>{}(entry.previous_reference.raw());
            result = combine(result, std::hash<std::uintptr_t>{}(entry.current_reference.raw()));
            result = combine(result, std::hash<const TimeSeriesRuntimeType *>{}(entry.runtime_type));
            const auto previous_time = entry.previous_last_updated.time().time_since_epoch().count();
            result = combine(result, std::hash<std::int64_t>{}(previous_time));
            result = combine(result, std::hash<bool>{}(entry.previous_last_updated.valid()));
            result = combine(result, std::hash<bool>{}(entry.previous_last_updated.became_valid()));
            result = combine(result, std::hash<bool>{}(entry.previous_last_updated.became_invalid()));
            result = combine(result, std::hash<bool>{}(entry.previous_last_updated.never_valid()));
            result = combine(result, std::hash<const void *>{}(entry.previous_storage));
            result = combine(result, std::hash<const TimeSeriesRuntimeType *>{}(entry.previous_runtime_type));
            return result;
        }
    };

    struct EntryEqual {
        bool operator()(const Entry &left, const Entry &right) const noexcept {
            const auto same_previous_state =
                left.previous_last_updated.time() == right.previous_last_updated.time() &&
                left.previous_last_updated.valid() == right.previous_last_updated.valid() &&
                left.previous_last_updated.became_valid() == right.previous_last_updated.became_valid() &&
                left.previous_last_updated.became_invalid() == right.previous_last_updated.became_invalid() &&
                left.previous_last_updated.never_valid() == right.previous_last_updated.never_valid();
            return left.previous_reference == right.previous_reference &&
                   left.current_reference == right.current_reference && left.runtime_type == right.runtime_type &&
                   same_previous_state &&
                   left.previous_storage == right.previous_storage &&
                   left.previous_runtime_type == right.previous_runtime_type;
        }
    };

    allocator *memory_;
    util::adaptive_set<Entry, 16, EntryHash, EntryEqual> entries_;
};

struct ReferenceInputSampleStorage {
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    ResolvedTimeSeries previous() const noexcept;
    void set(SampledReferenceChange sample) noexcept {
        assert(sample);
        assert(empty());
        sample.retain();
        const auto set_sample = sample.get<SampledSet>();
        const auto dictionary_sample = sample.get<SampledDictionary>();
        const auto pointer = reinterpret_cast<std::uintptr_t>(set_sample != nullptr
                                                                  ? static_cast<void *>(set_sample)
                                                                  : static_cast<void *>(dictionary_sample));
        assert((pointer & ~pointer_mask) == 0);
        bits_ = pointer | (bits_ & target_valid_flag) |
                (dictionary_sample != nullptr ? dictionary_sample_flag : std::uintptr_t{});
    }
    SampledReferenceChange sample() const noexcept {
        const auto pointer = reinterpret_cast<void *>(bits_ & pointer_mask);
        if (pointer == nullptr) {
            return {};
        }
        if ((bits_ & dictionary_sample_flag) != 0) {
            return SampledReferenceChange{static_cast<SampledDictionary *>(pointer)};
        }
        return SampledReferenceChange{static_cast<SampledSet *>(pointer)};
    }
    void clear() noexcept { clear_pointer(); }
    void reset() noexcept {
        clear_pointer();
        set_target_was_valid(false);
    }
    bool empty() const noexcept { return (bits_ & pointer_mask) == 0; }
    static constexpr std::size_t offset_for(std::size_t input_offset, bool dynamic_activity) noexcept {
        const auto after_input = input_offset + sizeof(ReferenceInputStorage) +
                                 (dynamic_activity ? sizeof(bool) : std::size_t{});
        return (after_input + alignof(ReferenceInputSampleStorage) - 1) &
               ~(alignof(ReferenceInputSampleStorage) - 1);
    }
    bool target_was_valid() const noexcept { return (bits_ & target_valid_flag) != 0; }
    void set_target_was_valid(bool valid) noexcept {
        bits_ = (bits_ & ~target_valid_flag) | (valid ? target_valid_flag : std::uintptr_t{});
    }
    void clear_pointer() noexcept {
        const auto previous = sample();
        bits_ &= target_valid_flag;
        previous.release();
    }

  private:
    static constexpr std::uintptr_t target_valid_flag = 1;
    static constexpr std::uintptr_t dictionary_sample_flag = 2;
    static constexpr std::uintptr_t pointer_mask = ~std::uintptr_t{3};

    std::uintptr_t bits_{};
};

static_assert(sizeof(ReferenceInputSampleStorage) == sizeof(void *));

} // namespace bg::details
