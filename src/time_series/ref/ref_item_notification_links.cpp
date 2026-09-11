#include "ref_item_notification_links.hpp"

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {

class ReferenceItemNotificationLinks {
    struct Link {
        std::uint32_t receiver_index;
        item_id_t receiver_item_id;

        bool operator==(const Link &) const = default;
    };

    struct NotificationReceiver {
        RuntimeGraph *graph;
        void *receiver;
        ReferenceItemNotifyFunction notify_function;
        ReferenceItemPublishFunction publish_function;
        std::uint32_t links{};
        bool touched{};
    };

    struct Group {
        explicit Group(std::pmr::memory_resource &memory) : links{&memory} {}

        std::pmr::vector<Link> links;
    };

  public:
    explicit ReferenceItemNotificationLinks(std::pmr::memory_resource &memory)
        : receivers_{&memory}, groups_{&memory}, touched_receivers_{&memory}, memory_{&memory} {}

    ~ReferenceItemNotificationLinks() {
        for (auto *group : groups_) {
            if (group == nullptr) {
                continue;
            }
            std::destroy_at(group);
            memory_->deallocate(group, sizeof(Group), alignof(Group));
        }
    }

    void add_root(RuntimeGraph &graph, void *receiver, item_id_t receiver_item_id,
                  ReferenceItemNotifyFunction notify_function, ReferenceItemPublishFunction publish_function) {
        add(root_notifications_, graph, receiver, receiver_item_id, notify_function, publish_function);
    }

    void add(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, void *receiver,
             item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function,
             ReferenceItemPublishFunction publish_function) {
        add(subscriptions.item_notifications(), graph, receiver, receiver_item_id, notify_function, publish_function);
    }

    void add(ItemNotificationDetail &notifications, RuntimeGraph &graph, void *receiver, item_id_t receiver_item_id,
             ReferenceItemNotifyFunction notify_function, ReferenceItemPublishFunction publish_function) {
        const auto receiver_count = receivers_.size();
        const auto receiver_index = require_receiver(graph, receiver, notify_function, publish_function);
        auto remove_unused_receiver = util::scope_exit{[&]() noexcept {
            if (receivers_.size() != receiver_count && receivers_.back().links == 0) {
                receivers_.pop_back();
            }
        }};
        const auto link = Link{receiver_index, receiver_item_id};

        if (notifications.empty()) {
            notifications.set_inline(link.receiver_index, link.receiver_item_id);
            ++receivers_[receiver_index].links;
            remove_unused_receiver.release();
            return;
        }
        if (!notifications.uses_overflow_group()) {
            if (inline_link(notifications) == link) {
                return;
            }

            auto *group = allocate_group();
            auto release_group = util::scope_exit{[&]() noexcept {
                std::destroy_at(group);
                memory_->deallocate(group, sizeof(Group), alignof(Group));
            }};
            group->links.push_back(inline_link(notifications));
            group->links.push_back(link);

            const auto group_index = store_group(group);
            release_group.release();
            notifications.set_overflow_group(group_index);
            ++receivers_[receiver_index].links;
            remove_unused_receiver.release();
            return;
        }

        auto &group = group_for(notifications);
        if (std::ranges::find(group.links, link) != group.links.end()) {
            return;
        }
        group.links.push_back(link);
        ++receivers_[receiver_index].links;
        remove_unused_receiver.release();
    }

    void remove_root(RuntimeGraph &graph, const void *receiver, item_id_t receiver_item_id,
                     ReferenceItemNotifyFunction notify_function) noexcept {
        remove(root_notifications_, graph, receiver, receiver_item_id, notify_function);
    }

    void remove(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, const void *receiver,
                item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function) noexcept {
        remove(subscriptions.item_notifications(), graph, receiver, receiver_item_id, notify_function);
    }

    void remove(ItemNotificationDetail &notifications, RuntimeGraph &graph, const void *receiver,
                item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function) noexcept {
        if (notifications.empty()) {
            return;
        }

        const auto matches = [&](Link link) {
            const auto &registered_receiver = receivers_[link.receiver_index];
            return link.receiver_item_id == receiver_item_id && registered_receiver.graph == &graph &&
                   registered_receiver.receiver == receiver && registered_receiver.notify_function == notify_function;
        };
        if (!notifications.uses_overflow_group()) {
            const auto link = inline_link(notifications);
            if (!matches(link)) {
                return;
            }
            --receivers_[link.receiver_index].links;
            notifications.clear();
            return;
        }

        auto &group = group_for(notifications);
        const auto found = std::ranges::find_if(group.links, matches);
        if (found == group.links.end()) {
            return;
        }
        --receivers_[found->receiver_index].links;
        *found = group.links.back();
        group.links.pop_back();
        if (group.links.size() == 1) {
            const auto remaining = group.links.front();
            release_group(notifications);
            notifications.set_inline(remaining.receiver_index, remaining.receiver_item_id);
        }
    }

    void notify_root(OutputChangeContext &context, OutputChange change) {
        notify(root_notifications_, context, change);
    }

    void notify(const ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context, OutputChange change) {
        notify(subscriptions.item_notifications(), context, change);
    }

    void notify(const ItemNotificationDetail &notifications, OutputChangeContext &context, OutputChange change) {
        if (notifications.empty()) {
            return;
        }
        if (!notifications.uses_overflow_group()) {
            notify(inline_link(notifications), context, change);
            return;
        }

        for (const auto link : group_for(notifications).links) {
            notify(link, context, change);
        }
    }

    bool empty(const ReferenceSubscriptionDetail &subscriptions) const noexcept {
        return subscriptions.item_notifications().empty();
    }

    void begin() noexcept { ++batch_depth_; }

    void cancel() noexcept {
        assert(batch_depth_ != 0);
        if (--batch_depth_ != 0) {
            return;
        }

        for (const auto index : touched_receivers_) {
            receivers_[index].touched = false;
        }
        touched_receivers_.clear();
    }

    void publish(OutputChangeContext &context) {
        assert(batch_depth_ != 0);
        if (batch_depth_ != 1) {
            --batch_depth_;
            return;
        }

        for (const auto index : touched_receivers_) {
            auto &receiver = receivers_[index];
            receiver.touched = false;
            auto receiver_context = context.for_graph(*receiver.graph);
            receiver.publish_function(receiver_context, receiver.receiver);
        }
        touched_receivers_.clear();
        batch_depth_ = 0;
    }

  private:
    static constexpr auto maximum_index = (std::uint32_t{1} << 31) - 1;

    std::uint32_t require_receiver(RuntimeGraph &graph, void *receiver, ReferenceItemNotifyFunction notify_function,
                                   ReferenceItemPublishFunction publish_function) {
        const auto existing = std::ranges::find_if(receivers_, [&](const auto &candidate) {
            return candidate.graph == &graph && candidate.receiver == receiver &&
                   candidate.notify_function == notify_function && candidate.publish_function == publish_function;
        });
        if (existing != receivers_.end()) {
            return static_cast<std::uint32_t>(existing - receivers_.begin());
        }

        const auto available = std::ranges::find_if(
            receivers_, [](const auto &candidate) { return candidate.links == 0 && !candidate.touched; });
        if (available != receivers_.end()) {
            *available = {&graph, receiver, notify_function, publish_function};
            return static_cast<std::uint32_t>(available - receivers_.begin());
        }
        if (receivers_.size() == maximum_index) {
            throw std::length_error{"reference item notification receiver table is full"};
        }

        receivers_.push_back({&graph, receiver, notify_function, publish_function});
        return static_cast<std::uint32_t>(receivers_.size() - 1);
    }

    Group *allocate_group() {
        auto *storage = memory_->allocate(sizeof(Group), alignof(Group));
        return std::construct_at(static_cast<Group *>(storage), *memory_);
    }

    std::uint32_t store_group(Group *group) {
        const auto available = std::ranges::find(groups_, nullptr);
        if (available != groups_.end()) {
            *available = group;
            return static_cast<std::uint32_t>(available - groups_.begin());
        }
        if (groups_.size() == maximum_index) {
            throw std::length_error{"reference item notification link group table is full"};
        }

        groups_.push_back(group);
        return static_cast<std::uint32_t>(groups_.size() - 1);
    }

    static Link inline_link(const ItemNotificationDetail &notifications) noexcept {
        return {notifications.receiver_index(), notifications.receiver_item_id()};
    }

    Group &group_for(const ItemNotificationDetail &notifications) const noexcept {
        return *groups_[notifications.overflow_group_index()];
    }

    void release_group(ItemNotificationDetail &notifications) noexcept {
        const auto index = notifications.overflow_group_index();
        auto *group = groups_[index];
        std::destroy_at(group);
        memory_->deallocate(group, sizeof(Group), alignof(Group));
        groups_[index] = nullptr;
    }

    void notify(Link link, OutputChangeContext &context, OutputChange change) {
        auto &receiver = receivers_[link.receiver_index];
        auto receiver_context = context.for_graph(*receiver.graph);
        receiver.notify_function(receiver_context, receiver.receiver, link.receiver_item_id, change);
        if (batch_depth_ == 0) {
            receiver.publish_function(receiver_context, receiver.receiver);
            return;
        }
        if (!receiver.touched) {
            touched_receivers_.push_back(link.receiver_index);
            receiver.touched = true;
        }
    }

    ItemNotificationDetail root_notifications_;
    std::pmr::vector<NotificationReceiver> receivers_;
    std::pmr::vector<Group *> groups_;
    std::pmr::vector<std::uint32_t> touched_receivers_;
    std::pmr::memory_resource *memory_;
    std::size_t batch_depth_{};
};

ReferenceItemNotificationLinks *create_reference_item_notification_links(std::pmr::memory_resource &memory) {
    auto *storage = memory.allocate(sizeof(ReferenceItemNotificationLinks), alignof(ReferenceItemNotificationLinks));
    auto release = util::scope_exit{[&]() noexcept {
        memory.deallocate(storage, sizeof(ReferenceItemNotificationLinks), alignof(ReferenceItemNotificationLinks));
    }};
    auto *result = std::construct_at(static_cast<ReferenceItemNotificationLinks *>(storage), memory);
    release.release();
    return result;
}

void destroy_reference_item_notification_links(ReferenceItemNotificationLinks *links,
                                               std::pmr::memory_resource &memory) noexcept {
    if (links == nullptr) {
        return;
    }

    std::destroy_at(links);
    memory.deallocate(links, sizeof(ReferenceItemNotificationLinks), alignof(ReferenceItemNotificationLinks));
}

void add_reference_root_item_link(ReferenceItemNotificationLinks &links, RuntimeGraph &graph, void *receiver,
                                  item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function,
                                  ReferenceItemPublishFunction publish_function) {
    links.add_root(graph, receiver, receiver_item_id, notify_function, publish_function);
}

void add_reference_item_link(ReferenceItemNotificationLinks &links, ReferenceSubscriptionDetail &subscriptions,
                             RuntimeGraph &graph, void *receiver, item_id_t receiver_item_id,
                             ReferenceItemNotifyFunction notify_function,
                             ReferenceItemPublishFunction publish_function) {
    links.add(subscriptions, graph, receiver, receiver_item_id, notify_function, publish_function);
}

void remove_reference_root_item_link(ReferenceItemNotificationLinks &links, RuntimeGraph &graph, const void *receiver,
                                     item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function) noexcept {
    links.remove_root(graph, receiver, receiver_item_id, notify_function);
}

void remove_reference_item_link(ReferenceItemNotificationLinks &links, ReferenceSubscriptionDetail &subscriptions,
                                RuntimeGraph &graph, const void *receiver, item_id_t receiver_item_id,
                                ReferenceItemNotifyFunction notify_function) noexcept {
    links.remove(subscriptions, graph, receiver, receiver_item_id, notify_function);
}

void notify_reference_root_item_links(ReferenceItemNotificationLinks &links, OutputChangeContext &context,
                                      OutputChange change) {
    links.notify_root(context, change);
}

void notify_reference_item_links(ReferenceItemNotificationLinks &links,
                                 const ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context,
                                 OutputChange change) {
    links.notify(subscriptions, context, change);
}

bool reference_item_links_empty(const ReferenceItemNotificationLinks &links,
                                const ReferenceSubscriptionDetail &subscriptions) noexcept {
    return links.empty(subscriptions);
}

void begin_reference_item_notification_batch(ReferenceItemNotificationLinks &links) noexcept {
    links.begin();
}

void cancel_reference_item_notification_batch(ReferenceItemNotificationLinks &links) noexcept {
    links.cancel();
}

void publish_reference_item_notification_batch(ReferenceItemNotificationLinks &links, OutputChangeContext &context) {
    links.publish(context);
}

} // namespace bg::details
