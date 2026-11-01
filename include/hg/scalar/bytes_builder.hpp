#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/bytes.hpp>
#include <bg/scalar/value.hpp>

#include <cstddef>
#include <span>

namespace bg {

namespace details {
class BytesStorage;
}

class BytesBuilder {
  public:
    BytesBuilder();
    explicit BytesBuilder(allocator &memory, std::size_t initial_capacity = 64);
    ~BytesBuilder();

    BytesBuilder(const BytesBuilder &) = delete;
    BytesBuilder &operator=(const BytesBuilder &) = delete;
    BytesBuilder(BytesBuilder &&other) noexcept;
    BytesBuilder &operator=(BytesBuilder &&other) noexcept;

    void reserve(std::size_t capacity);
    // The returned span is invalidated by reserve, append, move, or finish.
    std::span<std::byte> writable_tail() noexcept;
    // Commit the first `size` bytes of the current writable tail.
    void commit(std::size_t size);
    // Appending the builder's committed view is supported, including when growth is required.
    void append(std::span<const std::byte> value);

    std::span<const std::byte> view() const noexcept;
    [[nodiscard]] ScalarValue<bytes> finish() &&;

  private:
    details::BytesStorage *storage_{};
};

} // namespace bg
