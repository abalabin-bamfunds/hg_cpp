#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace bg::web::details {

inline bool valid_http_header_name(std::string_view value) noexcept {
    if (value.empty()) return false;
    constexpr auto punctuation = std::string_view{"!#$%&'*+-.^_`|~"};
    return std::ranges::all_of(value, [punctuation](unsigned char character) {
        if ((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z')) {
            return true;
        }
        return punctuation.find(static_cast<char>(character)) != std::string_view::npos;
    });
}

inline std::optional<std::string> utf8_header_value_to_latin1(std::string_view value) {
    auto result = std::string{};
    result.reserve(value.size());
    for (auto index = std::size_t{}; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        auto code_point = std::uint32_t{};
        auto length = std::size_t{};
        if (first < 0x80) {
            code_point = first;
            length = 1;
        } else if (first == 0xc2 || first == 0xc3) {
            if (index + 1 >= value.size()) return std::nullopt;
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if ((second & 0xc0) != 0x80) return std::nullopt;
            code_point = ((first & 0x1f) << 6) | (second & 0x3f);
            length = 2;
        } else {
            return std::nullopt;
        }
        if ((code_point < 0x20 && code_point != '\t') || code_point == 0x7f) return std::nullopt;
        result.push_back(static_cast<char>(code_point));
        index += length;
    }
    return result;
}

inline void append_latin1_header_value_as_utf8(std::string &result, std::string_view value) {
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x80) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back(static_cast<char>(0xc0 | (character >> 6)));
            result.push_back(static_cast<char>(0x80 | (character & 0x3f)));
        }
    }
}

} // namespace bg::web::details
