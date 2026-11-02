#pragma once

#include <cstddef>

namespace hg {

struct Size {
    std::size_t value;

    bool operator==(const Size &) const = default;
};

} // namespace hg
