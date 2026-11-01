#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/dynamic_node_subscriber_storage.hpp>
#include <bg/time_series/time_series_state.hpp>
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
class ItemNotifications;
struct RootReference;
struct ItemReference;
struct RepresentationReference;

} // namespace details

// ItemReference stores a Reference, so these alternatives are incomplete here.
// Their shared pointer alignment provides the two tag bits required for three alternatives.
using Reference = util::variant_ptr_with_alignment<alignof(void *), details::RootReference, details::ItemReference,
                                                   details::RepresentationReference>;

namespace details {

using RootNotifications = DynamicNodeSubscriberStorage<ReferenceProvider>;

struct ResolvedTimeSeries {
    static ResolvedTimeSeries resolve(Reference reference);

    bool valid() const noexcept {
        return runtime_type != nullptr && storage != nullptr && last_updated.valid();
    }

    TimeSeriesState last_updated{};
    const void *storage{};
    const TimeSeriesRuntimeType *runtime_type{};
};

class ItemNotificationDescriptor {
  public:
    bool empty() const noexcept { return location_ == 0; }
    bool has_destination_item_group() const noexcept { return (location_ & destination_item_group_flag) != 0; }

    std::uint32_t inline_destination_index() const noexcept {
        assert(!empty() && !has_destination_item_group());
        return location_ - 1;
    }

    item_id_t inline_destination_item_id() const noexcept {
        assert(!empty() && !has_destination_item_group());
        return destination_item_id_;
    }

    std::uint32_t destination_item_group_index() const noexcept {
        assert(has_destination_item_group());
        return (location_ & index_mask) - 1;
    }

    void set_inline_destination_item(std::uint32_t destination_index, item_id_t destination_item_id) noexcept {
        assert(destination_index < index_mask);
        location_ = destination_index + 1;
        destination_item_id_ = destination_item_id;
    }

    void set_destination_item_group(std::uint32_t group_index) noexcept {
        assert(group_index < index_mask);
        location_ = destination_item_group_flag | (group_index + 1);
        destination_item_id_ = 0;
    }

    void clear() noexcept {
        location_ = 0;
        destination_item_id_ = 0;
    }

  private:
    static constexpr auto destination_item_group_flag = std::uint32_t{1} << 31;
    static constexpr auto index_mask = destination_item_group_flag - 1;

    // Zero means no destination item. Otherwise this holds a one-based destination index for an inline item,
    // or a one-based destination-item-group index when the high bit is set.
    std::uint32_t location_{};
    item_id_t destination_item_id_{};
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

    ItemNotificationDescriptor &item_notification_descriptor() noexcept { return item_notification_descriptor_; }
    const ItemNotificationDescriptor &item_notification_descriptor() const noexcept {
        return item_notification_descriptor_;
    }

  private:
    std::uint32_t subscriber_group_{};
    ItemNotificationDescriptor item_notification_descriptor_;
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
    void set_input_node(const CompiledNode &node) noexcept { set_node(node, false); }
    void set_output_node(const CompiledNode &node) noexcept { set_node(node, true); }

    const CompiledNode &node() const noexcept {
        return *reinterpret_cast<const CompiledNode *>(node_and_output_flag_ & ~output_flag);
    }

    bool references_output() const noexcept { return (node_and_output_flag_ & output_flag) != 0; }

    bool has_inline_subscriber() const noexcept { return notifications_.destination.holds<RuntimeGraph>(); }
    bool has_dynamic_subscribers() const noexcept { return notifications_.destination.holds<DynamicSubscribers>(); }
    bool has_provider() const noexcept { return notifications_.destination.holds<ReferenceProvider>(); }

    RootNotifications::Destination notification_destination() const noexcept { return notifications_.destination; }

    DynamicNodeSubscriber inline_subscriber() const noexcept { return notifications_.inline_subscriber(); }

    DynamicSubscribers *dynamic_subscribers() const noexcept {
        return notifications_.destination.get<DynamicSubscribers>();
    }

    ReferenceProvider *provider() const noexcept { return notifications_.destination.get<ReferenceProvider>(); }

    void set_provider(ReferenceProvider &provider) noexcept {
        assert(!notifications_.destination || has_dynamic_subscribers());
        notifications_.destination = &provider;
    }

    void add_subscriber(DynamicNodeSubscriber subscriber);
    void remove_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept;
    void release_subscribers() noexcept;

    DynamicSubscribers &promote_subscribers();

    void clear_notifications() noexcept { notifications_.destination = nullptr; }

    RuntimeGraph *owner_graph{};

  private:
    void set_node(const CompiledNode &node, bool output) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(&node);
        assert((address & output_flag) == 0);
        assert(!notifications_.destination);
        node_and_output_flag_ = address | static_cast<std::uintptr_t>(output);
    }

    static constexpr auto output_flag = std::uintptr_t{1};

    std::uintptr_t node_and_output_flag_{};
    RootNotifications notifications_;
};

static_assert(sizeof(ResolvedTimeSeries) == 3 * sizeof(void *));
static_assert(sizeof(ItemNotificationDescriptor) == sizeof(void *));
static_assert(sizeof(ReferenceSubscriptionDetail) == 3 * sizeof(std::uint32_t));
static_assert(sizeof(ItemReference) == 3 * sizeof(void *));
static_assert(sizeof(RepresentationReference) == 3 * sizeof(void *));
static_assert(sizeof(RootNotifications) == 2 * sizeof(void *));
static_assert(sizeof(RootReference) == 4 * sizeof(void *));
static_assert(sizeof(Reference) == sizeof(void *));
static_assert(alignof(RootReference) >= 8);
static_assert(alignof(ItemReference) >= 8);
static_assert(alignof(RepresentationReference) >= 8);
static_assert(std::is_trivially_copyable_v<ReferenceSubscriptionDetail>);
static_assert(std::is_trivially_copyable_v<ItemReference>);
static_assert(std::is_trivially_copyable_v<RepresentationReference>);
static_assert(std::is_trivially_copyable_v<ItemNotificationDescriptor>);
static_assert(std::is_trivially_copyable_v<RootReference>);
static_assert(std::is_trivially_copyable_v<Reference>);

} // namespace details

} // namespace bg
