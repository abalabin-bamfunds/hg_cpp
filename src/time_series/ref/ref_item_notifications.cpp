#include "ref_item_notifications.hpp"

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <ranges>
#include <stdexcept>

namespace bg::details {

ItemNotifications::DestinationItemGroup::DestinationItemGroup(allocator memory) : items{&memory.memory_resource()} {}

ItemNotifications::ItemNotifications(allocator memory)
    : destinations_{&memory.memory_resource()}, destination_item_groups_{&memory.memory_resource()},
      pending_destinations_{&memory.memory_resource()}, memory_{memory} {}

ItemNotifications::~ItemNotifications() {
    for (auto *group : destination_item_groups_) {
        if (group == nullptr) {
            continue;
        }
        memory_.delete_object(group);
    }
}

void ItemNotifications::add_root(RuntimeGraph &graph, void *destination_representation, item_id_t destination_item_id,
                                 ItemChangeNotificationFunction notify_item_change,
                                 ItemChangePublicationFunction publish_item_changes) {
    add(root_item_notification_descriptor_, graph, destination_representation, destination_item_id, notify_item_change,
        publish_item_changes);
}

void ItemNotifications::add(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                            void *destination_representation, item_id_t destination_item_id,
                            ItemChangeNotificationFunction notify_item_change,
                            ItemChangePublicationFunction publish_item_changes) {
    add(subscriptions.item_notification_descriptor(), graph, destination_representation, destination_item_id,
        notify_item_change, publish_item_changes);
}

void ItemNotifications::add(ItemNotificationDescriptor &descriptor, RuntimeGraph &graph,
                            void *destination_representation, item_id_t destination_item_id,
                            ItemChangeNotificationFunction notify_item_change,
                            ItemChangePublicationFunction publish_item_changes) {
    const auto destination_count = destinations_.size();
    const auto destination_index =
        require_destination(graph, destination_representation, notify_item_change, publish_item_changes);
    auto remove_unused_destination = util::scope_exit{[&]() noexcept {
        if (destinations_.size() != destination_count && destinations_.back().descriptor_count == 0) {
            destinations_.pop_back();
        }
    }};
    const auto destination_item = DestinationItem{destination_index, destination_item_id};

    if (descriptor.empty()) {
        descriptor.set_inline_destination_item(destination_item.destination_index, destination_item.item_id);
        ++destinations_[destination_index].descriptor_count;
        remove_unused_destination.release();
        return;
    }
    if (!descriptor.has_destination_item_group()) {
        if (inline_destination_item(descriptor) == destination_item) {
            return;
        }

        auto *group = allocate_destination_item_group();
        auto release_group = util::scope_exit{[&]() noexcept { memory_.delete_object(group); }};
        group->items.push_back(inline_destination_item(descriptor));
        group->items.push_back(destination_item);

        const auto group_index = store_destination_item_group(group);
        release_group.release();
        descriptor.set_destination_item_group(group_index);
        ++destinations_[destination_index].descriptor_count;
        remove_unused_destination.release();
        return;
    }

    auto &group = destination_item_group_for(descriptor);
    if (std::ranges::find(group.items, destination_item) != group.items.end()) {
        return;
    }
    group.items.push_back(destination_item);
    ++destinations_[destination_index].descriptor_count;
    remove_unused_destination.release();
}

void ItemNotifications::remove_root(RuntimeGraph &graph, const void *destination_representation,
                                    item_id_t destination_item_id,
                                    ItemChangeNotificationFunction notify_item_change) noexcept {
    remove(root_item_notification_descriptor_, graph, destination_representation, destination_item_id,
           notify_item_change);
}

void ItemNotifications::remove(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                               const void *destination_representation, item_id_t destination_item_id,
                               ItemChangeNotificationFunction notify_item_change) noexcept {
    remove(subscriptions.item_notification_descriptor(), graph, destination_representation, destination_item_id,
           notify_item_change);
}

void ItemNotifications::remove(ItemNotificationDescriptor &descriptor, RuntimeGraph &graph,
                               const void *destination_representation, item_id_t destination_item_id,
                               ItemChangeNotificationFunction notify_item_change) noexcept {
    if (descriptor.empty()) {
        return;
    }

    const auto matches = [&](DestinationItem item) {
        const auto &registered_destination = destinations_[item.destination_index];
        return item.item_id == destination_item_id && registered_destination.graph == &graph &&
               registered_destination.destination_representation == destination_representation &&
               registered_destination.notify_item_change == notify_item_change;
    };
    if (!descriptor.has_destination_item_group()) {
        const auto item = inline_destination_item(descriptor);
        if (!matches(item)) {
            return;
        }
        --destinations_[item.destination_index].descriptor_count;
        descriptor.clear();
        return;
    }

    auto &group = destination_item_group_for(descriptor);
    const auto found = std::ranges::find_if(group.items, matches);
    if (found == group.items.end()) {
        return;
    }
    --destinations_[found->destination_index].descriptor_count;
    *found = group.items.back();
    group.items.pop_back();
    if (group.items.size() == 1) {
        const auto remaining = group.items.front();
        release_destination_item_group(descriptor);
        descriptor.set_inline_destination_item(remaining.destination_index, remaining.item_id);
    }
}

void ItemNotifications::notify_root(OutputChangeContext &context, OutputChange change) {
    notify(root_item_notification_descriptor_, context, change);
}

void ItemNotifications::notify(const ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context,
                               OutputChange change) {
    notify(subscriptions.item_notification_descriptor(), context, change);
}

void ItemNotifications::notify(const ItemNotificationDescriptor &descriptor, OutputChangeContext &context,
                               OutputChange change) {
    if (descriptor.empty()) {
        return;
    }
    if (!descriptor.has_destination_item_group()) {
        notify(inline_destination_item(descriptor), context, change);
        return;
    }

    for (const auto item : destination_item_group_for(descriptor).items) {
        notify(item, context, change);
    }
}

bool ItemNotifications::has_item_notifications(const ReferenceSubscriptionDetail &subscriptions) const noexcept {
    return !subscriptions.item_notification_descriptor().empty();
}

void ItemNotifications::begin_batch() noexcept {
    ++batch_depth_;
}

void ItemNotifications::cancel_batch() noexcept {
    assert(batch_depth_ != 0);
    if (--batch_depth_ != 0) {
        return;
    }

    for (const auto index : pending_destinations_) {
        destinations_[index].publication_pending = false;
    }
    pending_destinations_.clear();
}

void ItemNotifications::publish_batch(OutputChangeContext &context) {
    assert(batch_depth_ != 0);
    if (batch_depth_ != 1) {
        --batch_depth_;
        return;
    }

    for (const auto index : pending_destinations_) {
        auto &destination = destinations_[index];
        destination.publication_pending = false;
        auto destination_context = context.for_graph(*destination.graph);
        destination.publish_item_changes(destination_context, destination.destination_representation);
    }
    pending_destinations_.clear();
    batch_depth_ = 0;
}

std::uint32_t ItemNotifications::require_destination(
    RuntimeGraph &graph, void *destination_representation, ItemChangeNotificationFunction notify_item_change,
    ItemChangePublicationFunction publish_item_changes) {
    const auto existing = std::ranges::find_if(destinations_, [&](const auto &candidate) {
        return candidate.graph == &graph && candidate.destination_representation == destination_representation &&
               candidate.notify_item_change == notify_item_change &&
               candidate.publish_item_changes == publish_item_changes;
    });
    if (existing != destinations_.end()) {
        return static_cast<std::uint32_t>(existing - destinations_.begin());
    }

    const auto available = std::ranges::find_if(
        destinations_,
        [](const auto &candidate) { return candidate.descriptor_count == 0 && !candidate.publication_pending; });
    if (available != destinations_.end()) {
        *available = {&graph, destination_representation, notify_item_change, publish_item_changes};
        return static_cast<std::uint32_t>(available - destinations_.begin());
    }
    if (destinations_.size() == maximum_index) {
        throw std::length_error{"item notification destination table is full"};
    }

    destinations_.push_back({&graph, destination_representation, notify_item_change, publish_item_changes});
    return static_cast<std::uint32_t>(destinations_.size() - 1);
}

ItemNotifications::DestinationItemGroup *ItemNotifications::allocate_destination_item_group() {
    return memory_.new_object<DestinationItemGroup>(memory_);
}

std::uint32_t ItemNotifications::store_destination_item_group(DestinationItemGroup *group) {
    const auto available = std::ranges::find(destination_item_groups_, nullptr);
    if (available != destination_item_groups_.end()) {
        *available = group;
        return static_cast<std::uint32_t>(available - destination_item_groups_.begin());
    }
    if (destination_item_groups_.size() == maximum_index) {
        throw std::length_error{"item notification destination item group table is full"};
    }

    destination_item_groups_.push_back(group);
    return static_cast<std::uint32_t>(destination_item_groups_.size() - 1);
}

ItemNotifications::DestinationItem
ItemNotifications::inline_destination_item(const ItemNotificationDescriptor &descriptor) noexcept {
    return {descriptor.inline_destination_index(), descriptor.inline_destination_item_id()};
}

ItemNotifications::DestinationItemGroup &
ItemNotifications::destination_item_group_for(const ItemNotificationDescriptor &descriptor) const noexcept {
    return *destination_item_groups_[descriptor.destination_item_group_index()];
}

void ItemNotifications::release_destination_item_group(ItemNotificationDescriptor &descriptor) noexcept {
    const auto index = descriptor.destination_item_group_index();
    auto *group = destination_item_groups_[index];
    memory_.delete_object(group);
    destination_item_groups_[index] = nullptr;
}

void ItemNotifications::notify(DestinationItem item, OutputChangeContext &context, OutputChange change) {
    auto &destination = destinations_[item.destination_index];
    auto destination_context = context.for_graph(*destination.graph);
    destination.notify_item_change(destination_context, destination.destination_representation,
                                   item.item_id, change);
    if (batch_depth_ == 0) {
        destination.publish_item_changes(destination_context, destination.destination_representation);
        return;
    }
    if (!destination.publication_pending) {
        pending_destinations_.push_back(item.destination_index);
        destination.publication_pending = true;
    }
}

} // namespace bg::details
