#pragma once

#include <bg/allocator.hpp>

#include <cstddef>

namespace bg::details {

class ScalarAllocationPool;

struct ScalarAllocationOwner {
    allocator memory;
    ScalarAllocationPool *scalar_allocation_pool;
    const void *graph;
    std::size_t node_index;
};

class ScalarAllocationContext {
  public:
    ScalarAllocationContext(allocator memory, ScalarAllocationPool &scalar_allocation_pool, const void *graph,
                            std::size_t node_index) noexcept;
    ~ScalarAllocationContext();

    ScalarAllocationContext(const ScalarAllocationContext &) = delete;
    ScalarAllocationContext &operator=(const ScalarAllocationContext &) = delete;

  private:
    ScalarAllocationOwner owner_;
    ScalarAllocationOwner *previous_;
};

const ScalarAllocationOwner *current_scalar_allocation_owner() noexcept;
allocator &current_scalar_allocator();
ScalarAllocationPool *current_scalar_allocation_pool() noexcept;

void record_context_scalar_allocation(const void *value);
void check_context_scalar_deallocation(const void *value);

} // namespace bg::details
