#pragma once

#include <cstdint>

namespace bg {

class RuntimeGraph;

class ScheduleHandle {
  public:
    ScheduleHandle() = default;
    explicit operator bool() const noexcept { return id_ != 0; }

  private:
    friend RuntimeGraph;

    explicit ScheduleHandle(std::uint64_t id) noexcept : id_(id) {}

    std::uint64_t id_{};
};

} // namespace bg
