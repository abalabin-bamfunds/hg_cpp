#pragma once

#include <bg/optionality.hpp>
#include <bg/util/named_argument.hpp>

#include <cstdint>

namespace bg {

enum class InputActivity : std::uint8_t {
    active,
    passive,
};

inline constexpr InputActivity active = InputActivity::active;
inline constexpr InputActivity passive = InputActivity::passive;

using InputValidity = Optionality;

struct DynamicBinding {};

inline constexpr DynamicBinding dynamic_binding{};

struct ContextParameter {};

inline constexpr ContextParameter context{};

} // namespace bg
