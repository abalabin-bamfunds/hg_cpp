#include <bg/scalar/details/allocation_pool.hpp>

#include <bg/util/scope_exit.hpp>

#include <new>

namespace bg::details {

ScalarAllocationPool::ScalarAllocationPool(allocator upstream)
    : upstream_{upstream}, memory_{&upstream.memory_resource()} {}

ScalarAllocationPool *ScalarAllocationPool::create(allocator upstream) {
    auto *storage = static_cast<ScalarAllocationPool *>(
        upstream.allocate_bytes(sizeof(ScalarAllocationPool), alignof(ScalarAllocationPool)));
    auto rollback = util::scope_exit{[&]() noexcept {
        upstream.deallocate_bytes(storage, sizeof(ScalarAllocationPool), alignof(ScalarAllocationPool));
    }};
    auto *result = ::new (storage) ScalarAllocationPool{upstream};
    rollback.release();
    return result;
}

void ScalarAllocationPool::retain() noexcept {
    references_.fetch_add(1, std::memory_order_relaxed);
}

void ScalarAllocationPool::release() noexcept {
    if (references_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    auto upstream = upstream_;
    this->~ScalarAllocationPool();
    upstream.deallocate_bytes(this, sizeof(ScalarAllocationPool), alignof(ScalarAllocationPool));
}

} // namespace bg::details
