#include <bg/global_state.hpp>

#include <bg/scalar/bundle/details/allocation_owner.hpp>

namespace bg {

GlobalState::GlobalState() : GlobalState(allocator{}) {}

GlobalState::GlobalState(allocator upstream)
    : memory_{&upstream.memory_resource()}, allocator_{memory_},
      bundle_owner_{details::BundleAllocationOwner::create(upstream)} {}

GlobalState::~GlobalState() {
    values_.release(allocator_.memory_resource());
    bundle_owner_->release();
}

} // namespace bg
