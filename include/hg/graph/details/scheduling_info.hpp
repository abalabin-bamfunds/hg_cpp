#pragma once

#include <cstdint>
#include <type_traits>

namespace bg::details {

using ScheduleWord = std::uint64_t;

class SchedulingInfo {
  public:
    ScheduleWord input_validity() const noexcept { return inputs_valid_; }

    bool inputs_valid(ScheduleWord node_bit) const noexcept { return (inputs_valid_ & node_bit) != 0; }

    void input_validity(ScheduleWord value) noexcept { inputs_valid_ = value; }

    void inputs_valid(ScheduleWord node_bit, bool value) noexcept {
        if (value) {
            inputs_valid_ |= node_bit;
        } else {
            inputs_valid_ &= ~node_bit;
        }
    }

    void clear_requests() noexcept {
        evaluation = ScheduleWord{};
        validity_check = ScheduleWord{};
    }

    ScheduleWord evaluation{};
    ScheduleWord validity_check{};

  private:
    ScheduleWord inputs_valid_{};
};

static_assert(sizeof(SchedulingInfo) == 3 * sizeof(ScheduleWord));
static_assert(std::is_trivially_copyable_v<SchedulingInfo>);

} // namespace bg::details
