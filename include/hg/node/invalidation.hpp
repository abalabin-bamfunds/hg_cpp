#pragma once

#include <hg/graph/evaluation_function.hpp>

#include <cstdint>

namespace hg {

enum class InvalidationMode : std::uint8_t {
    invalidate_output,
    preserve_output,
    invoke,
};

struct InvalidationBehavior {
    InvalidationMode mode{InvalidationMode::invalidate_output};
    NodeFunction function{};
};

struct PreserveOutput {};

inline constexpr PreserveOutput preserve_output;

} // namespace hg
