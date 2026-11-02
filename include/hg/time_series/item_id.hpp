#pragma once

#include <cstdint>
#include <limits>

namespace hg {

using item_id_t = std::uint32_t;
inline constexpr auto key_set_item_id = std::numeric_limits<item_id_t>::max();

} // namespace hg
