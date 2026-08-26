#pragma once

#include <bg/util/compile_time_string.hpp>

#include <string>
#include <string_view>

namespace bg::util {

class symbol {
  public:
    explicit symbol(std::string_view text);

    [[nodiscard]] std::string_view text() const noexcept {
        return {text_->data(), text_->size()};
    }

    bool operator==(const symbol &) const noexcept = default;

  private:
    [[nodiscard]] static const std::string *intern(std::string_view text);

    const std::string *text_;
};

namespace literals {

template <compile_time_string Text>
[[nodiscard]] symbol operator""_sym() {
    static const symbol value{Text.view()};

    return value;
}

} // namespace literals

} // namespace bg::util
