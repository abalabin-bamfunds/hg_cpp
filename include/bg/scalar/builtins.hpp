#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/string.hpp>
#include <bg/util/symbol.hpp>

#include <tp/tpack.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace bg {

using byte = std::byte;
using i64 = std::int64_t;
using date_time_t = EvaluationTime;
using timedelta_t = std::chrono::microseconds;
using symbol = util::symbol;

namespace details {

// Drives generic handling of the atomic scalar declarations supplied by the engine.
inline constexpr auto builtin_scalar_declarations =
    tp::tpack_v<byte, bool, i64, double, date_time_t, timedelta_t, string, symbol>;

} // namespace details

} // namespace bg
