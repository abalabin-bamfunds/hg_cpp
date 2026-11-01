#pragma once

#include <bg/allocator.hpp>

#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <span>

namespace bg {

class ScalarData;
class BytesBuilder;

namespace details {

class ScalarAllocationPool;

class BytesStorage {
  public:
    BytesStorage(const BytesStorage &) = delete;
    BytesStorage &operator=(const BytesStorage &) = delete;

    [[nodiscard]] static BytesStorage *copy(std::span<const std::byte> value, allocator &memory);
    [[nodiscard]] static BytesStorage *copy(std::span<const std::byte> value);
    [[nodiscard]] static BytesStorage *from(const ScalarData &value) noexcept;

    std::span<const std::byte> value() const noexcept;
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }

    void retain() noexcept;
    void release() noexcept;

  private:
    friend class ::bg::BytesBuilder;

    BytesStorage(std::pmr::memory_resource &memory, ScalarAllocationPool *owner, std::size_t size,
                 std::size_t capacity) noexcept;

    [[nodiscard]] static BytesStorage *create_builder(std::size_t capacity, allocator &memory);
    [[nodiscard]] static BytesStorage *grow(BytesStorage &source, std::size_t capacity);
    [[nodiscard]] static BytesStorage *allocate(std::size_t size, std::size_t capacity, allocator &memory,
                                                ScalarAllocationPool *owner);
    static void deallocate(BytesStorage *storage) noexcept;
    const std::byte *data() const noexcept;
    std::byte *mutable_data() noexcept;
    void set_size(std::size_t size);

    std::atomic_size_t references_{1};
    std::pmr::memory_resource *memory_;
    ScalarAllocationPool *owner_;
    std::size_t size_;
    std::size_t capacity_;
};

} // namespace details
} // namespace bg
