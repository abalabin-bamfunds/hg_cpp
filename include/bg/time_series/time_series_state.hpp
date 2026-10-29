#pragma once

#include <bg/evaluation_time.hpp>

#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>

namespace bg {

// Stores the latest change of a time series. The upper bits record validity
// transitions; ordinary valid updates retain the EvaluationTime bit pattern.
class TimeSeriesState {
  public:
    constexpr TimeSeriesState() = default;
    constexpr explicit TimeSeriesState(EvaluationTime time) noexcept { set_normal(time); }

    constexpr bool valid() const noexcept { return std::bit_cast<std::int64_t>(bits_) > 0; }
    constexpr bool became_valid() const noexcept { return transition() == valid_flag; }
    constexpr bool became_invalid() const noexcept { return transition() == invalid_flag; }
    constexpr bool never_valid() const noexcept { return bits_ == never_valid_bits; }
    constexpr bool valid_before(EvaluationTime time) const noexcept {
        return valid() ^ ((this->time() == time) & (became_valid() | became_invalid()));
    }
    constexpr bool later_than(const TimeSeriesState &other) const noexcept {
        // Shifting out the flags compares only the packed timestamp.
        return (bits_ << 2) > (other.bits_ << 2);
    }

    constexpr EvaluationTime time() const noexcept { return EvaluationTime{EvaluationTime::duration{time_count()}}; }
    static constexpr TimeSeriesState from_validity_transition(EvaluationTime time, bool was_valid,
                                                              bool is_valid) noexcept {
        if (time == MIN_DT) {
            assert(!was_valid && !is_valid);
            return {};
        }
        auto result = TimeSeriesState{};
        result.bits_ = time_count(time) | (static_cast<std::uint64_t>(!is_valid) << 63) |
                       (static_cast<std::uint64_t>(!was_valid) << 62);
        return result;
    }
    static constexpr bool can_represent(EvaluationTime time) noexcept {
        const auto count = time.time_since_epoch().count();
        return count > 0 && static_cast<std::uint64_t>(count) <= time_mask;
    }

    constexpr void publish(EvaluationTime time) noexcept {
        assert(can_represent(time));
        const auto count = time_count(time);
        // Preserve a first-tick transition until a later time is published.
        const auto became_valid = bits_ >= (valid_flag | count);
        bits_ = count | (static_cast<std::uint64_t>(became_valid) << 62);
    }

    constexpr void invalidate(EvaluationTime time) noexcept {
        assert(valid());
        assert(can_represent(time));
        bits_ = time_count(time) | invalid_flag;
    }

    constexpr void update_collection_validity(EvaluationTime time, bool is_valid) noexcept {
        const auto was_valid = valid_before(time);
        *this = from_validity_transition(time, was_valid, is_valid);
    }

  private:
    static constexpr std::uint64_t time_mask = (std::uint64_t{1} << 62) - 1;
    static constexpr std::uint64_t valid_flag = std::uint64_t{1} << 62;
    static constexpr std::uint64_t invalid_flag = std::uint64_t{1} << 63;
    // `11` records a stable invalid state; an invalidation has only the invalid flag.
    static constexpr std::uint64_t never_valid_bits = invalid_flag | valid_flag;

    constexpr std::uint64_t time_count() const noexcept { return bits_ & time_mask; }
    static constexpr std::uint64_t time_count(EvaluationTime time) noexcept {
        const auto count = time.time_since_epoch().count();
        assert(can_represent(time));
        return static_cast<std::uint64_t>(count);
    }
    constexpr std::uint64_t transition() const noexcept { return bits_ & ~time_mask; }

    constexpr void set_normal(EvaluationTime time) noexcept {
        bits_ = time == MIN_DT ? never_valid_bits : time_count(time);
    }

    std::uint64_t bits_{never_valid_bits};
};

static_assert(sizeof(TimeSeriesState) == sizeof(EvaluationTime));
static_assert(alignof(TimeSeriesState) == alignof(EvaluationTime));
static_assert(std::is_trivially_copyable_v<TimeSeriesState>);

inline constexpr TimeSeriesState initial_time_series_state{};

} // namespace bg
