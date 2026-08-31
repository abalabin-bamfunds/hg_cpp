#pragma once

#include <bg/evaluation_time.hpp>

#include <type_traits>

namespace bg {

class RuntimeGraph;

namespace details {

struct DynamicSubscribers;
struct ReferenceInputStorage;

struct ReferenceStorage {
    RuntimeGraph *owner{};
    const EvaluationTime *target_last_updated{};
    const void *target_value{};
    DynamicSubscribers *subscribers{};
};

struct ReferenceInputStorage {
    ReferenceStorage *target{};
};

static_assert(sizeof(ReferenceStorage) == 4 * sizeof(void *));
static_assert(sizeof(ReferenceInputStorage) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<ReferenceStorage>);
static_assert(std::is_trivially_copyable_v<ReferenceInputStorage>);

} // namespace details
} // namespace bg
