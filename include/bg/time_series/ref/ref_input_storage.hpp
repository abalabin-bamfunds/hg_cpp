#pragma once

#include <bg/time_series/ref/ref_storage.hpp>

#include <bit>
#include <cstdint>
#include <type_traits>

namespace bg {

struct DeferredReferenceInputState {
    Reference reference{};
    TimeSeriesState last_updated{};
};

// Stores the current reference selected for one dereferencing node input.
struct ReferenceInputStorage {
    Reference reference() const noexcept {
        const auto reference_tag = bits_ & reference_tag_mask;
        const auto reference_bits = bits_ & ~selected_view_valid_flag;
        if (reference_tag == deferred_reference_tag) {
            return deferred_state()->reference;
        }
        if ((reference_bits & reference_pointer_mask) == 0) {
            assert((reference_bits & ~reference_tag_mask) == 0);
            return {};
        }
        assert(reference_tag < 3);
        return std::bit_cast<Reference>(reference_bits);
    }

    bool has_deferred_state() const noexcept { return (bits_ & reference_tag_mask) == deferred_reference_tag; }

    DeferredReferenceInputState *deferred_state() const noexcept {
        assert(has_deferred_state());
        return reinterpret_cast<DeferredReferenceInputState *>(bits_ & ~reference_storage_tag_mask);
    }

    bool selected_view_was_valid() const noexcept { return (bits_ & selected_view_valid_flag) != 0; }

    TimeSeriesState observed_state(const TimeSeriesState &reference_state,
                                   const details::ResolvedTimeSeries &target) noexcept {
        if (has_deferred_state()) {
            const auto &retained_state = deferred_state()->last_updated;
            if (target.last_updated.later_than(reference_state)) {
                const auto last_updated = target.last_updated;
                release_deferred(selected_view_was_valid());
                return last_updated;
            }
            if (target.last_updated.later_than(retained_state)) {
                return target.last_updated;
            }

            return TimeSeriesState::from_validity_transition(
                retained_state.time(), selected_view_was_valid(), retained_state.valid() && target.valid());
        }
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

    void set_deferred(DeferredReferenceInputState &state, bool selected_view_valid) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(&state);
        assert((address & reference_storage_tag_mask) == 0);
        bits_ = address | deferred_reference_tag |
                (selected_view_valid ? selected_view_valid_flag : std::uintptr_t{});
    }

    void replace_deferred(DeferredReferenceInputState &state, bool selected_view_valid) noexcept;

    void release_deferred(bool selected_view_valid) noexcept;

  private:
    static constexpr std::uintptr_t reference_tag_mask = 3;
    static constexpr std::uintptr_t deferred_reference_tag = reference_tag_mask;
    static constexpr std::uintptr_t selected_view_valid_flag = std::uintptr_t{1} << 2;
    static constexpr std::uintptr_t reference_storage_tag_mask = reference_tag_mask | selected_view_valid_flag;
    static constexpr std::uintptr_t reference_pointer_mask = ~reference_tag_mask;

    std::uintptr_t bits_{};
};

static_assert(sizeof(DeferredReferenceInputState) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<DeferredReferenceInputState>);
static_assert(sizeof(ReferenceInputStorage) == sizeof(void *));
static_assert(alignof(ReferenceInputStorage) >= 4);
static_assert(std::is_trivially_copyable_v<ReferenceInputStorage>);
static_assert(sizeof(Reference) == sizeof(std::uintptr_t));
static_assert(alignof(ReferenceInputStorage) >= 8);

} // namespace bg
