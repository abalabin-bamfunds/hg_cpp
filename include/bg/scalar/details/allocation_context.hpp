#pragma once

#include <cstddef>
#include <memory_resource>

namespace bg::details {

struct ScalarAllocationOwner {
    std::pmr::memory_resource *memory;
    const void *graph;
    std::size_t node_index;
};

class ScalarAllocationContext {
  public:
    ScalarAllocationContext(std::pmr::memory_resource &memory, const void *graph, std::size_t node_index) noexcept;
    ~ScalarAllocationContext();

    ScalarAllocationContext(const ScalarAllocationContext &) = delete;
    ScalarAllocationContext &operator=(const ScalarAllocationContext &) = delete;

  private:
    ScalarAllocationOwner owner_;
    const ScalarAllocationOwner *previous_;
};

[[nodiscard]] const ScalarAllocationOwner *current_scalar_allocation_owner() noexcept;

void record_context_scalar_allocation(const void *value);
void check_context_scalar_deallocation(const void *value);

} // namespace bg::details
