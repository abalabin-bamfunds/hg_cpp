#pragma once

#include <cstdint>

namespace bg {

struct ScheduleHandle {
    explicit operator bool() const noexcept { return id != 0; }

    std::uint32_t slot{};
    std::uint32_t id{};
};

} // namespace bg
