#include <bg/scalar/bundle/details/allocation_owner.hpp>

#include <bg/util/scope_exit.hpp>

#include <new>

namespace bg::details {

BundleAllocationOwner::BundleAllocationOwner(allocator upstream)
    : upstream_{upstream}, memory_{&upstream.memory_resource()} {}

BundleAllocationOwner *BundleAllocationOwner::create(allocator upstream) {
    auto *storage = static_cast<BundleAllocationOwner *>(
        upstream.allocate_bytes(sizeof(BundleAllocationOwner), alignof(BundleAllocationOwner)));
    auto rollback = util::scope_exit{[&]() noexcept {
        upstream.deallocate_bytes(storage, sizeof(BundleAllocationOwner), alignof(BundleAllocationOwner));
    }};
    auto *result = ::new (storage) BundleAllocationOwner{upstream};
    rollback.release();
    return result;
}

void BundleAllocationOwner::retain() noexcept {
    references_.fetch_add(1, std::memory_order_relaxed);
}

void BundleAllocationOwner::release() noexcept {
    if (references_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    auto upstream = upstream_;
    this->~BundleAllocationOwner();
    upstream.deallocate_bytes(this, sizeof(BundleAllocationOwner), alignof(BundleAllocationOwner));
}

} // namespace bg::details
