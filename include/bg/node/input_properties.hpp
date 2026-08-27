#pragma once

#include <bg/util/compile_time_string.hpp>

#include <cstdint>

namespace bg {

enum class InputActivity : std::uint8_t {
    active,
    passive,
};

inline constexpr InputActivity active = InputActivity::active;
inline constexpr InputActivity passive = InputActivity::passive;

enum class InputValidity : std::uint8_t {
    required,
    optional,
};

inline constexpr InputValidity required = InputValidity::required;
inline constexpr InputValidity optional = InputValidity::optional;

namespace details {

template <util::compile_time_string Text> struct compile_time_name {
    static constexpr auto text = Text;
};

} // namespace details

namespace literals {

template <util::compile_time_string Text> [[nodiscard]] consteval auto operator""_n() {
    return details::compile_time_name<Text>{};
}

} // namespace literals

} // namespace bg
