#pragma once

#include <cstddef>
#include <limits>

namespace bg {

// Identifies either a local wired output or an inherited context requirement.
class WiredInput {
  public:
    static WiredInput output(std::size_t index) noexcept { return WiredInput{index}; }

    static WiredInput context(std::size_t index) noexcept { return WiredInput{index | context_flag}; }

    bool is_context() const noexcept { return (value_ & context_flag) != 0; }

    std::size_t index() const noexcept { return value_ & index_mask; }

    bool operator==(const WiredInput &) const noexcept = default;

  private:
    static constexpr auto context_flag = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    static constexpr auto index_mask = context_flag - 1;

    explicit WiredInput(std::size_t value) noexcept : value_{value} {}

    std::size_t value_{};
};

static_assert(sizeof(WiredInput) == sizeof(std::size_t));

} // namespace bg
