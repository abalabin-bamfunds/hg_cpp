#pragma once

#include <bg/time_series/ref/ref_storage.hpp>

#include <bit>
#include <cstdint>
#include <type_traits>

namespace bg {

// Stores the current reference selected for one dereferencing node input.
struct ReferenceInputStorage {
    Reference reference() const noexcept {
        const auto reference_tag = bits_ & reference_tag_mask;
        const auto reference_bits = bits_ & ~selected_view_valid_flag;
        if ((reference_bits & reference_pointer_mask) == 0) {
            assert(reference_bits == 0);
            return {};
        }
        assert(reference_tag < 3);
        return std::bit_cast<Reference>(reference_bits);
    }

    bool selected_view_was_valid() const noexcept { return (bits_ & selected_view_valid_flag) != 0; }

    TimeSeriesState observed_state(const TimeSeriesState &reference_state,
                                   const details::ResolvedTimeSeries &target) const noexcept {
        if (target.last_updated.later_than(reference_state)) {
            return target.last_updated;
        }

        return TimeSeriesState::from_validity_transition(reference_state.time(), selected_view_was_valid(),
                                                         reference_state.valid() && target.valid());
    }

    void set(Reference reference, bool selected_view_valid) noexcept {
        const auto reference_bits = reference.raw();
        assert((reference_bits & selected_view_valid_flag) == 0);
        assert(!reference || (reference_bits & reference_tag_mask) < 3);
        bits_ = reference_bits | (selected_view_valid ? selected_view_valid_flag : std::uintptr_t{});
    }

  private:
    static constexpr std::uintptr_t reference_tag_mask = 3;
    static constexpr std::uintptr_t reference_pointer_mask = ~reference_tag_mask;
    static constexpr std::uintptr_t selected_view_valid_flag = std::uintptr_t{1} << 2;

    std::uintptr_t bits_{};
};

static_assert(sizeof(ReferenceInputStorage) == sizeof(void *));
static_assert(alignof(ReferenceInputStorage) >= 4);
static_assert(std::is_trivially_copyable_v<ReferenceInputStorage>);
static_assert(sizeof(Reference) == sizeof(std::uintptr_t));
static_assert(alignof(ReferenceInputStorage) >= 8);

} // namespace bg
