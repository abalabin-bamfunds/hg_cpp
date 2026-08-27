#pragma once

#include <bg/util/symbol.hpp>

#include <type_traits>

namespace bg {

struct WiringPath {
    const WiringPath *parent;
    util::symbol name;
    util::symbol label;
};

static_assert(std::is_trivially_destructible_v<WiringPath>);

} // namespace bg
