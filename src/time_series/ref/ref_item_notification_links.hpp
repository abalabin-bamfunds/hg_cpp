#pragma once

#include <bg/time_series/ref/ref_provider.hpp>

namespace bg::details {

ReferenceItemNotificationLinks *create_reference_item_notification_links(std::pmr::memory_resource &memory);
void destroy_reference_item_notification_links(ReferenceItemNotificationLinks *links,
                                               std::pmr::memory_resource &memory) noexcept;

void add_reference_root_item_link(ReferenceItemNotificationLinks &links, RuntimeGraph &graph, void *receiver,
                                  item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function,
                                  ReferenceItemPublishFunction publish_function);
void add_reference_item_link(ReferenceItemNotificationLinks &links, ReferenceSubscriptionDetail &subscriptions,
                             RuntimeGraph &graph, void *receiver, item_id_t receiver_item_id,
                             ReferenceItemNotifyFunction notify_function,
                             ReferenceItemPublishFunction publish_function);
void remove_reference_root_item_link(ReferenceItemNotificationLinks &links, RuntimeGraph &graph, const void *receiver,
                                     item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function) noexcept;
void remove_reference_item_link(ReferenceItemNotificationLinks &links, ReferenceSubscriptionDetail &subscriptions,
                                RuntimeGraph &graph, const void *receiver, item_id_t receiver_item_id,
                                ReferenceItemNotifyFunction notify_function) noexcept;

void notify_reference_root_item_links(ReferenceItemNotificationLinks &links, OutputChangeContext &context,
                                      OutputChange change);
void notify_reference_item_links(ReferenceItemNotificationLinks &links,
                                 const ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context,
                                 OutputChange change);
bool reference_item_links_empty(const ReferenceItemNotificationLinks &links,
                                const ReferenceSubscriptionDetail &subscriptions) noexcept;

void begin_reference_item_notification_batch(ReferenceItemNotificationLinks &links) noexcept;
void cancel_reference_item_notification_batch(ReferenceItemNotificationLinks &links) noexcept;
void publish_reference_item_notification_batch(ReferenceItemNotificationLinks &links, OutputChangeContext &context);

} // namespace bg::details
