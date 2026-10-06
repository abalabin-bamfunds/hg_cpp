#pragma once

#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cassert>
#include <cstdint>

namespace bg::details {

struct SampledSet;

struct ReferenceInputSampleStorage {
    TimeSeriesRuntimeType::ConstItem input() const noexcept;
    ResolvedTimeSeries previous() const noexcept;
    void set(SampledSet *sample) noexcept { set_pointer(sample); }
    SampledSet *set_sample() const noexcept {
        return empty() ? nullptr : pointer<SampledSet>();
    }
    void clear() noexcept;
    void reset() noexcept {
        clear();
        set_target_was_valid(false);
    }
    bool empty() const noexcept { return (bits_ & pointer_mask) == 0; }
    bool target_was_valid() const noexcept { return (bits_ & target_valid_flag) != 0; }
    void set_target_was_valid(bool valid) noexcept {
        bits_ = (bits_ & ~target_valid_flag) | (valid ? target_valid_flag : std::uintptr_t{});
    }
    void clear_pointer() noexcept { bits_ &= target_valid_flag; }

  private:
    static constexpr std::uintptr_t target_valid_flag = 1;
    static constexpr std::uintptr_t pointer_mask = ~target_valid_flag;

    template <typename T> T *pointer() const noexcept {
        return reinterpret_cast<T *>(bits_ & pointer_mask);
    }

    void set_pointer(SampledSet *sample) noexcept {
        assert((reinterpret_cast<std::uintptr_t>(sample) & ~pointer_mask) == 0);
        bits_ = reinterpret_cast<std::uintptr_t>(sample) | (bits_ & target_valid_flag);
    }

    std::uintptr_t bits_{};
};

static_assert(sizeof(ReferenceInputSampleStorage) == sizeof(void *));

inline constexpr std::size_t reference_input_sample_offset(std::size_t input_offset,
                                                           bool dynamic_activity) noexcept {
    const auto after_input = input_offset + sizeof(ReferenceInputStorage) +
                             (dynamic_activity ? sizeof(bool) : std::size_t{});
    return (after_input + alignof(ReferenceInputSampleStorage) - 1) &
           ~(alignof(ReferenceInputSampleStorage) - 1);
}

} // namespace bg::details
