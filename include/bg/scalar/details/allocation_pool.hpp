#pragma once

#include <bg/allocator.hpp>

#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace bg::details {

// Owns the thread-safe pool shared by scalar values with nested dynamic storage.
class ScalarAllocationPool {
  public:
    [[nodiscard]] static ScalarAllocationPool *create(allocator upstream);

    ScalarAllocationPool(const ScalarAllocationPool &) = delete;
    ScalarAllocationPool &operator=(const ScalarAllocationPool &) = delete;

    void retain() noexcept;
    void release() noexcept;

    allocator memory() noexcept { return allocator{memory_}; }

  private:
    explicit ScalarAllocationPool(allocator upstream);
    ~ScalarAllocationPool() = default;

    std::atomic_size_t references_{1};
    allocator upstream_;
    std::pmr::synchronized_pool_resource memory_;
};

} // namespace bg::details
