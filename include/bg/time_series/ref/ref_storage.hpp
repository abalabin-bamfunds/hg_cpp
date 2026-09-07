#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace bg {

class RuntimeGraph;
class CompiledNode;
class TimeSeriesRuntimeType;

namespace details {

struct DynamicSubscribers;
class ReferenceProvider;
struct ReferenceInputStorage;
struct ReferenceRoot;
struct ReferenceRoute;

using ReferenceAddress = util::erased_tagged_variant_ptr<alignof(void *), ReferenceRoot, ReferenceRoute>;
using ReferenceRootData = util::erased_tagged_variant_ptr<alignof(void *), DynamicSubscribers, ReferenceProvider>;

struct ReferenceTarget {
    const EvaluationTime *last_updated{};
    const void *value{};
    const TimeSeriesRuntimeType *runtime_type{};
};

struct ReferenceRoute {
    void bind_item(ReferenceAddress parent_address, TimeSeriesItemId item) noexcept {
        assert(item < representation_flag);
        parent = parent_address;
        target = item;
    }

    void bind_representation(ReferenceAddress source, std::uint32_t index) noexcept {
        assert(index < representation_flag);
        parent = source;
        target = representation_flag | index;
    }

    bool is_representation() const noexcept { return (target & representation_flag) != 0; }
    TimeSeriesItemId item() const noexcept {
        assert(!is_representation());
        return target;
    }
    std::uint32_t representation() const noexcept {
        assert(is_representation());
        return target & ~representation_flag;
    }

    ReferenceAddress parent;
    std::uint32_t target{};
    std::uint32_t subscriber_group{};

  private:
    static constexpr auto representation_flag = std::uint32_t{1} << 31;
};

struct ReferenceRoot {
    RuntimeGraph *graph{};
    const CompiledNode *source{};
    const TimeSeriesRuntimeType *runtime_type{};
    ReferenceRootData data;
};

struct ReferenceInputStorage {
    ReferenceAddress target;
};

ReferenceTarget reference_target(ReferenceAddress address);

static_assert(sizeof(ReferenceTarget) == 3 * sizeof(void *));
static_assert(sizeof(ReferenceRoute) == 2 * sizeof(void *));
static_assert(sizeof(ReferenceRoot) == 4 * sizeof(void *));
static_assert(sizeof(ReferenceAddress) == sizeof(void *));
static_assert(sizeof(ReferenceInputStorage) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<ReferenceRoute>);
static_assert(std::is_trivially_copyable_v<ReferenceRoot>);
static_assert(std::is_trivially_copyable_v<ReferenceAddress>);
static_assert(std::is_trivially_copyable_v<ReferenceInputStorage>);

} // namespace details
} // namespace bg
