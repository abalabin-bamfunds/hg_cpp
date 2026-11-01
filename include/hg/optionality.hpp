#pragma once

#include <cstdint>

namespace bg {

enum class Optionality : std::uint8_t {
    required,
    optional,
};

inline constexpr Optionality required = Optionality::required;
inline constexpr Optionality optional = Optionality::optional;

} // namespace bg
