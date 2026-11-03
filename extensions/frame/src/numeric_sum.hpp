#pragma once

#include <hg/frame/column.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hg::frame::details {

// Shared arithmetic for column and grouped sums. Owners cache the individual row contributions.
template <numeric_cell_type T> class NumericSum;

template <> class NumericSum<i64> {
  public:
    i64 result() const noexcept { return contribution_count_ == 0 ? i64{} : total_; }

    void add(i64 value) noexcept {
        ++contribution_count_;
        total_ = std::bit_cast<i64>(static_cast<std::uint64_t>(total_) + static_cast<std::uint64_t>(value));
    }

    void remove(i64 value) noexcept {
        --contribution_count_;
        total_ = std::bit_cast<i64>(static_cast<std::uint64_t>(total_) - static_cast<std::uint64_t>(value));
        if (contribution_count_ == 0)
            total_ = 0;
    }

  private:
    i64 total_{};
    std::size_t contribution_count_{};
};

template <> class NumericSum<double> {
  public:
    double result() const noexcept {
        if (contribution_count_ == 0)
            return 0;
        if (nan_count_ != 0 || (positive_infinity_count_ != 0 && negative_infinity_count_ != 0))
            return std::numeric_limits<double>::quiet_NaN();
        if (positive_infinity_count_ != 0)
            return std::numeric_limits<double>::infinity();
        if (negative_infinity_count_ != 0)
            return -std::numeric_limits<double>::infinity();
        return finite_sum_ + finite_correction_;
    }

    void add(double value) noexcept {
        ++contribution_count_;
        if (std::isnan(value)) {
            ++nan_count_;
        } else if (std::isinf(value)) {
            if (std::signbit(value))
                ++negative_infinity_count_;
            else
                ++positive_infinity_count_;
        } else {
            ++finite_count_;
            accumulate_finite(value);
        }
    }

    void remove(double value) noexcept {
        --contribution_count_;
        if (std::isnan(value)) {
            --nan_count_;
        } else if (std::isinf(value)) {
            if (std::signbit(value))
                --negative_infinity_count_;
            else
                --positive_infinity_count_;
        } else {
            --finite_count_;
            if (finite_count_ == 0) {
                reset_finite_accumulator();
            } else if (!finite_overflow_) {
                accumulate_finite(-value);
            }
        }
    }

    bool finite_overflowed() const noexcept { return finite_overflow_; }

    // Rebuild operations change the finite accumulator only. Contribution and non-finite counts remain unchanged.
    void reset_finite_accumulator() noexcept {
        finite_sum_ = 0;
        finite_correction_ = 0;
        finite_overflow_ = false;
    }

    void add_finite_contribution(double value) noexcept {
        if (std::isfinite(value))
            accumulate_finite(value);
    }

    template <typename Scan> void recompute_finite_if_overflowed(Scan &&scan) noexcept {
        if (!finite_overflow_)
            return;
        reset_finite_accumulator();
        scan([&](double value) { add_finite_contribution(value); });
    }

  private:
    void set_finite_overflow(double value) noexcept {
        finite_sum_ = std::copysign(std::numeric_limits<double>::infinity(), value);
        finite_correction_ = 0;
        finite_overflow_ = true;
    }

    void accumulate_finite(double value) noexcept {
        if (finite_overflow_)
            return;

        const auto next_sum = finite_sum_ + value;
        if (!std::isfinite(next_sum)) {
            set_finite_overflow(next_sum);
            return;
        }

        const auto sum_is_larger = std::abs(finite_sum_) >= std::abs(value);
        const auto difference = sum_is_larger ? finite_sum_ - next_sum : value - next_sum;
        const auto adjustment = sum_is_larger ? difference + value : difference + finite_sum_;

        const auto next_correction = finite_correction_ + adjustment;
        if (!std::isfinite(next_correction)) {
            set_finite_overflow(next_correction);
            return;
        }
        finite_sum_ = next_sum;
        finite_correction_ = next_correction;
    }

    std::size_t contribution_count_{};
    std::size_t finite_count_{};
    std::size_t positive_infinity_count_{};
    std::size_t negative_infinity_count_{};
    std::size_t nan_count_{};
    double finite_sum_{};
    double finite_correction_{};
    bool finite_overflow_{};
};

} // namespace hg::frame::details
