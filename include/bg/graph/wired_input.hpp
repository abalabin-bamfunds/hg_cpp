#pragma once

#include <cstddef>
#include <limits>

namespace bg {

// Identifies a local output, inherited time series requirement, or borrowed input slot.
class WiredInput {
  public:
    static WiredInput output(std::size_t index, bool dereference = false) noexcept {
        return WiredInput{index | (dereference ? dereference_flag : 0)};
    }

    static WiredInput inherited(std::size_t index, bool dereference = false) noexcept {
        return WiredInput{index | inherited_flag | (dereference ? dereference_flag : 0)};
    }

    static WiredInput borrowed(std::size_t index) noexcept { return WiredInput{index | borrowed_flag}; }

    bool is_inherited() const noexcept { return (value_ & inherited_flag) != 0; }
    bool dereferences_reference() const noexcept { return (value_ & dereference_flag) != 0; }

    bool is_borrowed() const noexcept { return (value_ & (inherited_flag | borrowed_flag)) == borrowed_flag; }

    bool is_output() const noexcept { return (value_ & (inherited_flag | borrowed_flag)) == 0; }

    std::size_t index() const noexcept { return value_ & index_mask; }

    bool operator==(const WiredInput &) const noexcept = default;

  private:
    static constexpr auto inherited_flag = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    static constexpr auto dereference_flag = inherited_flag >> 1;
    static constexpr auto borrowed_flag = inherited_flag >> 2;
    static constexpr auto index_mask = borrowed_flag - 1;

    explicit WiredInput(std::size_t value) noexcept : value_{value} {}

    std::size_t value_{};
};

static_assert(sizeof(WiredInput) == sizeof(std::size_t));

} // namespace bg
