#include <bg/graph/graph_instance_id.hpp>

#include <atomic>
#include <limits>
#include <stdexcept>

namespace bg {
namespace {

std::atomic<std::uint64_t> next_graph_instance_id{1};

} // namespace

GraphInstanceId GraphInstanceIdAllocator::allocate() { return reserve_ids(1); }

GraphInstanceIdRange GraphInstanceIdAllocator::reserve(std::uint64_t count) {
    if (count == 0) {
        throw std::invalid_argument{"runtime graph instance ID range must be non-empty"};
    }
    return GraphInstanceIdRange{reserve_ids(count)};
}

GraphInstanceId GraphInstanceIdAllocator::reserve_ids(std::uint64_t count) {
    auto first = next_graph_instance_id.load(std::memory_order_relaxed);
    for (;;) {
        if (count > std::numeric_limits<std::uint64_t>::max() - first) {
            throw std::length_error{"runtime graph instance ID space exhausted"};
        }
        if (next_graph_instance_id.compare_exchange_weak(first, first + count, std::memory_order_relaxed)) {
            return {first};
        }
    }
}

} // namespace bg
