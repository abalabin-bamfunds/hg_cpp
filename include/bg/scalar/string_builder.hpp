#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/string.hpp>
#include <bg/scalar/value.hpp>

#include <cstddef>
#include <string_view>

namespace bg {

namespace details {
class StringStorage;
}

class StringBuilder {
  public:
    StringBuilder();
    explicit StringBuilder(allocator &memory, std::size_t initial_capacity = 64);
    ~StringBuilder();

    StringBuilder(const StringBuilder &) = delete;
    StringBuilder &operator=(const StringBuilder &) = delete;
    StringBuilder(StringBuilder &&other) noexcept;
    StringBuilder &operator=(StringBuilder &&other) noexcept;

    void reserve(std::size_t capacity);
    void append(std::string_view value);
    void append(char value);

    std::string_view view() const noexcept;
    [[nodiscard]] ScalarValue<string> finish() &&;

  private:
    details::StringStorage *storage_{};
};

} // namespace bg
