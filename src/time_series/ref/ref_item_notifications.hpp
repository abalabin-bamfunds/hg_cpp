#pragma once

#include <bg/time_series/ref/ref_provider.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace bg::details {

class ItemNotifications {
    struct DestinationItem {
        std::uint32_t destination_index;
        item_id_t item_id;

        bool operator==(const DestinationItem &) const = default;
    };

    struct ItemNotificationDestination {
        RuntimeGraph *graph;
        void *destination_representation;
        ItemChangeNotificationFunction notify_item_change;
        ItemChangePublicationFunction publish_item_changes;
        std::uint32_t descriptor_count{};
        bool publication_pending{};
    };

    struct DestinationItemGroup {
        explicit DestinationItemGroup(allocator memory);

        std::pmr::vector<DestinationItem> items;
    };

  public:
    explicit ItemNotifications(allocator memory);
    ~ItemNotifications();

    void add_root(RuntimeGraph &graph, void *destination_representation, item_id_t destination_item_id,
                  ItemChangeNotificationFunction notify_item_change,
                  ItemChangePublicationFunction publish_item_changes);
    void add(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, void *destination_representation,
             item_id_t destination_item_id, ItemChangeNotificationFunction notify_item_change,
             ItemChangePublicationFunction publish_item_changes);
    void remove_root(RuntimeGraph &graph, const void *destination_representation, item_id_t destination_item_id,
                     ItemChangeNotificationFunction notify_item_change) noexcept;
    void remove(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                const void *destination_representation, item_id_t destination_item_id,
                ItemChangeNotificationFunction notify_item_change) noexcept;
    void notify_root(OutputChangeContext &context, OutputChange change);
    void notify(const ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context, OutputChange change);
    bool has_item_notifications(const ReferenceSubscriptionDetail &subscriptions) const noexcept;
    template <typename IsInternalDestination>
    bool has_external_item_notifications(const ReferenceSubscriptionDetail &subscriptions,
                                         IsInternalDestination is_internal_destination) const noexcept {
        const auto &descriptor = subscriptions.item_notification_descriptor();
        if (descriptor.empty()) {
            return false;
        }

        const auto is_external = [&](DestinationItem item) {
            return !is_internal_destination(destinations_[item.destination_index].destination_representation);
        };
        if (!descriptor.has_destination_item_group()) {
            return is_external(inline_destination_item(descriptor));
        }
        for (const auto item : destination_item_group_for(descriptor).items) {
            if (is_external(item)) {
                return true;
            }
        }
        return false;
    }
    void begin_batch() noexcept;
    void cancel_batch() noexcept;
    void publish_batch(OutputChangeContext &context);

  private:
    static constexpr auto maximum_index = (std::uint32_t{1} << 31) - 1;

    void add(ItemNotificationDescriptor &descriptor, RuntimeGraph &graph, void *destination_representation,
             item_id_t destination_item_id, ItemChangeNotificationFunction notify_item_change,
             ItemChangePublicationFunction publish_item_changes);
    void remove(ItemNotificationDescriptor &descriptor, RuntimeGraph &graph, const void *destination_representation,
                item_id_t destination_item_id, ItemChangeNotificationFunction notify_item_change) noexcept;
    void notify(const ItemNotificationDescriptor &descriptor, OutputChangeContext &context, OutputChange change);
    void notify(DestinationItem item, OutputChangeContext &context, OutputChange change);
    std::uint32_t require_destination(RuntimeGraph &graph, void *destination_representation,
                                      ItemChangeNotificationFunction notify_item_change,
                                      ItemChangePublicationFunction publish_item_changes);
    DestinationItemGroup *allocate_destination_item_group();
    std::uint32_t store_destination_item_group(DestinationItemGroup *group);
    static DestinationItem inline_destination_item(const ItemNotificationDescriptor &descriptor) noexcept;
    DestinationItemGroup &destination_item_group_for(const ItemNotificationDescriptor &descriptor) const noexcept;
    void release_destination_item_group(ItemNotificationDescriptor &descriptor) noexcept;

    ItemNotificationDescriptor root_item_notification_descriptor_;
    std::pmr::vector<ItemNotificationDestination> destinations_;
    std::pmr::vector<DestinationItemGroup *> destination_item_groups_;
    std::pmr::vector<std::uint32_t> pending_destinations_;
    allocator memory_;
    std::size_t batch_depth_{};
};

} // namespace bg::details
