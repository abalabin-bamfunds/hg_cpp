#pragma once

#include <hg/allocator.hpp>
#include <hg/scalar/string.hpp>
#include <hg/scalar/value.hpp>

#include <cstddef>
#include <string_view>

namespace hg {

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

} // namespace hg
