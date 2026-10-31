#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
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

inline std::string encode_latin1_header_value(std::string_view value, std::size_t first_non_ascii) {
    const auto non_ascii_count = static_cast<std::size_t>(std::ranges::count_if(
        value.substr(first_non_ascii), [](unsigned char character) { return character >= 0x80; }));
    auto result = std::string{};
    if (value.size() > result.max_size() || non_ascii_count > result.max_size() - value.size()) {
        throw std::length_error{"UTF-8 header value is too large"};
    }
    const auto output_size = value.size() + non_ascii_count;
    result.resize_and_overwrite(output_size, [&](char *output, std::size_t) {
        std::copy_n(value.data(), first_non_ascii, output);

        auto output_index = first_non_ascii;
        for (auto input_index = first_non_ascii; input_index < value.size(); ++input_index) {
            const auto character = static_cast<unsigned char>(value[input_index]);
            if (character < 0x80) {
                output[output_index++] = static_cast<char>(character);
                continue;
            }
            output[output_index++] = static_cast<char>(0xc0 | (character >> 6));
            output[output_index++] = static_cast<char>(0x80 | (character & 0x3f));
        }
        return output_size;
    });
    return result;
}

inline std::string latin1_header_value_as_utf8(std::string_view value) {
    const auto first_non_ascii = std::ranges::find_if(value, [](unsigned char character) { return character >= 0x80; });
    if (first_non_ascii == value.end()) return std::string{value};
    return encode_latin1_header_value(value, static_cast<std::size_t>(std::distance(value.begin(), first_non_ascii)));
}

inline void convert_latin1_header_value_as_utf8(std::string &value) {
    const auto first_non_ascii =
        std::ranges::find_if(value, [](unsigned char character) { return character >= 0x80; });
    if (first_non_ascii == value.end()) return;
    auto converted = encode_latin1_header_value(
        value, static_cast<std::size_t>(std::distance(value.begin(), first_non_ascii)));
    value.swap(converted);
}

} // namespace bg::web::details
