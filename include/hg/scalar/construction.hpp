#pragma once

#include <bg/allocator.hpp>

#include <cstddef>

namespace bg {

class ScalarRuntimeType;

// Engine and scalar-provider interface. Borrows callback context while constructing one scalar value synchronously.
// The callback receives uninitialized storage with the runtime type's size and alignment. It must complete
// construction before returning. If it throws, the storage must contain no live object or owned resources.
// The context must remain valid until the callback returns.
struct ScalarConstruction {
    void *context;
    void (*construct)(void *context, const ScalarRuntimeType &runtime_type, void *destination, allocator &memory);
};

// Constructs scalar fields synchronously by index in runtime-selected destinations. Return true only when the
// destination contains a complete value. Return false or throw only when it contains no live object or owned
// resources. The context and all borrowed inputs must remain valid until the callback returns.
struct ScalarFieldConstruction {
    void *context;
    bool (*construct)(void *context, std::size_t field_index, const ScalarRuntimeType &runtime_type,
                      void *destination, allocator &memory);
};

} // namespace bg
