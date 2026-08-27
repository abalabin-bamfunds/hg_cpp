#pragma once

#include <bg/graph/evaluation_function.hpp>

#include <cstdint>

namespace bg {

enum class InvalidationMode : std::uint8_t {
    invalidate_output,
    preserve_output,
    invoke,
};

struct InvalidationBehavior {
    InvalidationMode mode{InvalidationMode::invalidate_output};
    EvaluationFunction function{};
};

struct PreserveOutput {};

inline constexpr PreserveOutput preserve_output;

} // namespace bg
