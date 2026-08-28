#include <bg/scalar/details/allocation_context.hpp>

#include "../thread_local_pointer.hpp"

#include <cassert>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace bg::details {
namespace {

[[nodiscard]] util::thread_local_pointer &current_owner() {
    static auto value = util::thread_local_pointer{};

    return value;
}

#ifndef NDEBUG
std::mutex allocations_mutex;
std::unordered_map<const void *, ScalarAllocationOwner> allocations;
#endif

} // namespace

ScalarAllocationContext::ScalarAllocationContext(std::pmr::memory_resource &memory, const void *graph,
                                                 std::size_t node_index) noexcept
    : owner_{&memory, graph, node_index}, previous_{current_scalar_allocation_owner()} {
    current_owner().set(&owner_);
}

ScalarAllocationContext::~ScalarAllocationContext() {
    assert(current_scalar_allocation_owner() == &owner_);
    current_owner().set(const_cast<ScalarAllocationOwner *>(previous_));
}

const ScalarAllocationOwner *current_scalar_allocation_owner() noexcept {
    return static_cast<const ScalarAllocationOwner *>(current_owner().get());
}

std::pmr::memory_resource &current_scalar_memory_resource() {
    const auto *owner = current_scalar_allocation_owner();
    if (owner == nullptr) {
        throw std::logic_error{"scalar mutation requires an allocation context"};
    }

    return *owner->memory;
}

void record_context_scalar_allocation(const void *value) {
#ifndef NDEBUG
    const auto *owner = current_scalar_allocation_owner();
    assert(owner != nullptr);

    const auto lock = std::scoped_lock{allocations_mutex};
    const auto inserted = allocations.emplace(value, *owner).second;
    assert(inserted);
#else
    (void)value;
#endif
}

void check_context_scalar_deallocation(const void *value) {
#ifndef NDEBUG
    const auto *owner = current_scalar_allocation_owner();
    assert(owner != nullptr);

    const auto lock = std::scoped_lock{allocations_mutex};
    const auto allocation = allocations.find(value);
    assert(allocation != allocations.end());
    assert(allocation->second.memory == owner->memory);
    assert(allocation->second.graph == owner->graph);
    assert(allocation->second.node_index == owner->node_index);
    allocations.erase(allocation);
#else
    (void)value;
#endif
}

} // namespace bg::details
