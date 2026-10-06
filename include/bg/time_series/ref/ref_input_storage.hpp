#pragma once

#include <bg/time_series/ref/ref_storage.hpp>

#include <type_traits>

namespace bg {

// Stores the current reference selected for one dereferencing node input.
struct ReferenceInputStorage {
    Reference value;
};

static_assert(sizeof(ReferenceInputStorage) == sizeof(void *));
static_assert(alignof(ReferenceInputStorage) >= 4);
static_assert(std::is_trivially_copyable_v<ReferenceInputStorage>);

} // namespace bg
