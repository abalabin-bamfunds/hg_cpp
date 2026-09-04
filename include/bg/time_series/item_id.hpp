#pragma once

#include <cstdint>
#include <limits>

namespace bg {

using TimeSeriesItemId = std::uint32_t;
inline constexpr auto key_set_item_id = std::numeric_limits<TimeSeriesItemId>::max();

} // namespace bg
