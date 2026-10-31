#pragma once

#include <chrono>
#include <cstdint>

namespace bg {

using EvaluationTime = std::chrono::sys_time<std::chrono::microseconds>;

inline constexpr EvaluationTime MIN_DT{};

static_assert(sizeof(EvaluationTime) == sizeof(std::int64_t));

} // namespace bg
