#include <bg/global_state.hpp>

#include <bg/scalar/details/allocation_pool.hpp>

namespace bg {

GlobalState::GlobalState() : GlobalState(allocator{}) {}

GlobalState::GlobalState(allocator upstream)
    : memory_{&upstream.memory_resource()}, allocator_{memory_},
      scalar_allocation_pool_{details::ScalarAllocationPool::create(upstream)} {}

GlobalState::~GlobalState() {
    values_.release(allocator_.memory_resource());
    scalar_allocation_pool_->release();
}

} // namespace bg
