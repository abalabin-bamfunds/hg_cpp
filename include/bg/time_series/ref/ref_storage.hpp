#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace bg {

class RuntimeGraph;
class CompiledNode;
class TimeSeriesRuntimeType;

namespace details {

struct DynamicSubscribers;
class ReferenceProvider;
class ReferenceItemNotificationLinks;
struct RootReference;
struct ItemReference;
struct RepresentationReference;

} // namespace details

// ItemReference stores a Reference, so these alternatives are incomplete here.
// Their shared pointer alignment provides the two tag bits required for three alternatives.
using Reference = util::variant_ptr_with_alignment<alignof(void *), details::RootReference, details::ItemReference,
                                                   details::RepresentationReference>;

namespace details {

// Stores direct root notifications until a provider is required. The provider
// then owns the notifications and representation state for this root.
using RootReferenceNotificationStorage =
    util::variant_ptr_with_alignment<alignof(void *), DynamicSubscribers, ReferenceProvider>;

struct ResolvedTimeSeries {
    bool valid() const noexcept {
        return runtime_type != nullptr && storage != nullptr && last_updated != nullptr && *last_updated > MIN_DT;
    }

    const EvaluationTime *last_updated{};
    const void *storage{};
    const TimeSeriesRuntimeType *runtime_type{};
};

class ItemNotificationDetail {
  public:
    bool empty() const noexcept { return location_ == 0; }
    bool uses_overflow_group() const noexcept { return (location_ & overflow_group_flag) != 0; }

    std::uint32_t receiver_index() const noexcept {
        assert(!empty() && !uses_overflow_group());
        return location_ - 1;
    }

    item_id_t receiver_item_id() const noexcept {
        assert(!empty() && !uses_overflow_group());
        return receiver_item_id_;
    }

    std::uint32_t overflow_group_index() const noexcept {
        assert(uses_overflow_group());
        return (location_ & index_mask) - 1;
    }

    void set_inline(std::uint32_t receiver_index, item_id_t receiver_item_id) noexcept {
        assert(receiver_index < index_mask);
        location_ = receiver_index + 1;
        receiver_item_id_ = receiver_item_id;
    }

    void set_overflow_group(std::uint32_t group_index) noexcept {
        assert(group_index < index_mask);
        location_ = overflow_group_flag | (group_index + 1);
        receiver_item_id_ = 0;
    }

    void clear() noexcept {
        location_ = 0;
        receiver_item_id_ = 0;
    }

  private:
    static constexpr auto overflow_group_flag = std::uint32_t{1} << 31;
    static constexpr auto index_mask = overflow_group_flag - 1;

    // Zero means no link. Otherwise the low bits are a one-based receiver index
    // for one inline link, or a one-based overflow-group index when the high bit is set.
    std::uint32_t location_{};
    item_id_t receiver_item_id_{};
};

class ReferenceSubscriptionDetail {
  public:
    bool has_subscriber_group() const noexcept { return subscriber_group_ != 0; }

    std::uint32_t subscriber_group_index() const noexcept {
        assert(has_subscriber_group());
        return subscriber_group_ - 1;
    }

    void set_subscriber_group(std::uint32_t index) noexcept {
        assert(index != std::numeric_limits<std::uint32_t>::max());
        subscriber_group_ = index + 1;
    }

    void clear_subscriber_group() noexcept { subscriber_group_ = 0; }

    ItemNotificationDetail &item_notifications() noexcept { return item_notifications_; }
    const ItemNotificationDetail &item_notifications() const noexcept { return item_notifications_; }

  private:
    std::uint32_t subscriber_group_{};
    ItemNotificationDetail item_notifications_;
};

struct ItemReference {
    ItemReference() = default;
    ItemReference(Reference parent, item_id_t item_id) noexcept : parent_{parent}, item_id_{item_id} {}

    Reference parent() const noexcept { return parent_; }
    item_id_t item_id() const noexcept { return item_id_; }
    ReferenceSubscriptionDetail &subscriptions() noexcept { return subscriptions_; }
    const ReferenceSubscriptionDetail &subscriptions() const noexcept { return subscriptions_; }

  private:
    Reference parent_;
    item_id_t item_id_{};
    ReferenceSubscriptionDetail subscriptions_;
};

struct RepresentationReference {
    RepresentationReference() = default;
    RepresentationReference(Reference base, std::uint32_t representation_index) noexcept
        : base_{base}, representation_index_{representation_index} {}

    Reference base() const noexcept { return base_; }
    std::uint32_t representation_index() const noexcept { return representation_index_; }
    ReferenceSubscriptionDetail &subscriptions() noexcept { return subscriptions_; }
    const ReferenceSubscriptionDetail &subscriptions() const noexcept { return subscriptions_; }

  private:
    Reference base_;
    std::uint32_t representation_index_{};
    ReferenceSubscriptionDetail subscriptions_;
};

struct RootReference {
    RuntimeGraph *owner_graph{};
    const CompiledNode *reference_node{};
    const TimeSeriesRuntimeType *referenced_runtime_type{};
    RootReferenceNotificationStorage subscribers_or_provider;
};

ResolvedTimeSeries resolve_reference(Reference reference);

static_assert(sizeof(ResolvedTimeSeries) == 3 * sizeof(void *));
static_assert(sizeof(ItemNotificationDetail) == sizeof(void *));
static_assert(sizeof(ReferenceSubscriptionDetail) == 3 * sizeof(std::uint32_t));
static_assert(sizeof(ItemReference) == 3 * sizeof(void *));
static_assert(sizeof(RepresentationReference) == 3 * sizeof(void *));
static_assert(sizeof(RootReference) == 4 * sizeof(void *));
static_assert(sizeof(Reference) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<ReferenceSubscriptionDetail>);
static_assert(std::is_trivially_copyable_v<ItemReference>);
static_assert(std::is_trivially_copyable_v<RepresentationReference>);
static_assert(std::is_trivially_copyable_v<ItemNotificationDetail>);
static_assert(std::is_trivially_copyable_v<RootReference>);
static_assert(std::is_trivially_copyable_v<Reference>);

} // namespace details

} // namespace bg
