#pragma once

#include <cstddef>

namespace bg {

struct Size {
    std::size_t value;

    bool operator==(const Size &) const = default;
};

} // namespace bg
