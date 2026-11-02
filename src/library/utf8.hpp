#pragma once

#include <hg/scalar/builtins.hpp>

#include <unicode/utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hg {
class StringBuilder;
}

namespace hg::operators::details {

enum class CaseConversion { lower, upper, fold };

class Utf8View {
  public:
    explicit Utf8View(std::string_view text) : text_{text} {
        auto byte_index = std::size_t{};
        while (byte_index < text.size()) {
            const auto remaining = text.size() - byte_index;
            const auto chunk_size =
                std::min(remaining, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
            // Leave room for a four-byte code point at the end of each non-final chunk.
            const auto chunk_end = remaining > chunk_size ? chunk_size - 3 : chunk_size;
            auto chunk_index = std::int32_t{};

            while (static_cast<std::size_t>(chunk_index) < chunk_end) {
                auto code_point = UChar32{};
                U8_NEXT(text.data() + byte_index, chunk_index, static_cast<std::int32_t>(chunk_size), code_point);
                if (code_point < 0) {
                    throw std::invalid_argument{"string contains malformed UTF-8"};
                }
                ++length_;
            }
            byte_index += static_cast<std::size_t>(chunk_index);
        }
    }

    explicit Utf8View(symbol text) : Utf8View{text.text()} {}

    std::size_t length() const noexcept { return length_; }

    std::string_view at(std::int64_t index) const {
        auto position = std::size_t{};
        if (index < 0) {
            const auto distance = static_cast<std::uint64_t>(-(index + 1)) + 1;
            if (distance > length_) {
                throw std::out_of_range{"string index is out of range"};
            }
            position = length_ - static_cast<std::size_t>(distance);
        } else {
            const auto positive_index = static_cast<std::uint64_t>(index);
            if (positive_index >= length_) {
                throw std::out_of_range{"string index is out of range"};
            }
            position = static_cast<std::size_t>(positive_index);
        }

        const auto first = byte_offset(position);
        auto last = first;
        U8_FWD_1_UNSAFE(text_.data(), last);
        return text_.substr(first, last - first);
    }

    std::string_view slice(std::int64_t start, std::int64_t end) const {
        const auto first = clamp(start);
        const auto last = clamp(end);
        if (last <= first) {
            return {};
        }

        const auto first_byte = byte_offset(first);
        auto last_byte = first_byte;
        for (auto code_point = first; code_point < last; ++code_point) {
            U8_FWD_1_UNSAFE(text_.data(), last_byte);
        }
        return text_.substr(first_byte, last_byte - first_byte);
    }

    void append_case_conversion(CaseConversion conversion, StringBuilder &builder) const;

  private:
    std::size_t clamp(std::int64_t index) const noexcept {
        if (index < 0) {
            const auto distance = static_cast<std::uint64_t>(-(index + 1)) + 1;
            return distance > length_ ? 0 : length_ - static_cast<std::size_t>(distance);
        }

        return static_cast<std::size_t>(std::min(static_cast<std::uint64_t>(index),
                                                 static_cast<std::uint64_t>(length_)));
    }

    std::size_t byte_offset(std::size_t code_point_index) const noexcept {
        auto byte_index = std::size_t{};
        for (auto code_point = std::size_t{}; code_point < code_point_index; ++code_point) {
            U8_FWD_1_UNSAFE(text_.data(), byte_index);
        }
        return static_cast<std::size_t>(byte_index);
    }

    std::string_view text_;
    std::size_t length_{};
};

} // namespace hg::operators::details
