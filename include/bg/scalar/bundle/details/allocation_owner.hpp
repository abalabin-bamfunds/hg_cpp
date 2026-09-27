#pragma once

#include <bg/allocator.hpp>

#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace bg::details {

// Owns the thread-safe pool shared by a group of hierarchical scalar values.
class BundleAllocationOwner {
  public:
    [[nodiscard]] static BundleAllocationOwner *create(allocator upstream);

    BundleAllocationOwner(const BundleAllocationOwner &) = delete;
    BundleAllocationOwner &operator=(const BundleAllocationOwner &) = delete;

    void retain() noexcept;
    void release() noexcept;

    allocator memory() noexcept { return allocator{memory_}; }

  private:
    explicit BundleAllocationOwner(allocator upstream);
    ~BundleAllocationOwner() = default;

    std::atomic_size_t references_{1};
    allocator upstream_;
    std::pmr::synchronized_pool_resource memory_;
};

} // namespace bg::details
