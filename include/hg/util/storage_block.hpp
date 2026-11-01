#pragma once

#include <cstdint>

namespace bg::util {

// Describes raw storage owned by a block. data remains valid while its owning bunch is unchanged and does not imply
// that any cell in the block is constructed.
// The 64-bit end preserves the exclusive bound 2^32 for the final block.
struct storage_block {
    std::uint64_t first;
    std::uint64_t end;
    const void *data;
};

} // namespace bg::util
