#pragma once

#include <cstddef>
#include <string_view>

namespace bg::util {

template <std::size_t Size> struct compile_time_string {
    char value[Size];

    consteval compile_time_string(const char (&text)[Size]) {
        for (std::size_t i = 0; i < Size; ++i) {
            value[i] = text[i];
        }
    }

    constexpr const char *data() const noexcept { return value; }

    constexpr std::size_t size() const noexcept { return Size - 1; }

    constexpr std::string_view view() const noexcept { return {value, size()}; }

    template <std::size_t OtherSize>
    constexpr bool operator==(const compile_time_string<OtherSize> &other) const noexcept {
        return view() == other.view();
    }
};

template <std::size_t Size> compile_time_string(const char (&)[Size]) -> compile_time_string<Size>;

} // namespace bg::util
