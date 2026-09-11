#include <bg/time_series/ref/ref_provider.hpp>

#include "ref_item_notification_links.hpp"

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <limits>
#include <memory>
#include <ranges>

namespace bg::details {

ReferenceRepresentation::~ReferenceRepresentation() = default;
ReferenceProviderComponent::~ReferenceProviderComponent() = default;

namespace {

ReferenceSubscriptionDetail &reference_subscriptions(Reference reference) noexcept {
    if (auto *item = reference.get<ItemReference>()) {
        return item->subscriptions();
    }
    return reference.get<RepresentationReference>()->subscriptions();
}

Reference parent_reference(Reference reference) noexcept {
    if (const auto *item = reference.get<ItemReference>()) {
        return item->parent();
    }
    if (const auto *representation = reference.get<RepresentationReference>()) {
        return representation->base();
    }
    return {};
}

} // namespace

ReferenceProvider::ReferenceProvider(allocator memory, RootReference &root,
                                     DynamicSubscribers *root_subscribers)
    : memory_{memory}, root_{&root}, representations_{&memory.memory_resource()},
      subscriber_groups_{&memory.memory_resource()} {
    subscriber_groups_.push_back(root_subscribers);
}

ReferenceProvider::~ReferenceProvider() {
    for (auto *component = components_; component != nullptr; component = component->next_) {
        component->detach();
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        remove_reference_observer(representation->base_reference(), owner(), representation);
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        representation->destroy(memory_);
    }
    while (components_ != nullptr) {
        auto *component = components_;
        components_ = component->next_;
        component->destroy(memory_);
    }
    for (auto *subscribers : subscriber_groups_) {
        DynamicSubscribers::release(subscribers);
    }
    destroy_reference_item_notification_links(item_notification_links_, memory_);
}

ResolvedTimeSeries ReferenceProvider::resolve_representation(std::uint32_t index) const {
    assert(representations_[index] != nullptr);
    return representations_[index]->resolve();
}

void ReferenceProvider::add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t callback_data,
                                            std::uint32_t node_index, bool active, bool required,
                                            InputChangeFunction callback) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), memory_);
    subscribers.add(graph, input_offset, callback_data, node_index, active, required, callback);
}

void ReferenceProvider::add_root_observer(RuntimeGraph &graph, void *receiver, DynamicObserverFunction function) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), memory_);
    subscribers.add_observer(graph, receiver, function);
}

void ReferenceProvider::remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (subscriber_groups_.front() != nullptr) {
        subscriber_groups_.front()->remove(graph, input_offset);
    }
}

void ReferenceProvider::remove_root_observer(RuntimeGraph &graph, const void *receiver) noexcept {
    if (subscriber_groups_.front() != nullptr) {
        subscriber_groups_.front()->remove_observer(graph, receiver);
    }
}

void ReferenceProvider::notify_root(OutputChangeContext &context, OutputChange change) {
    context.notify(subscriber_groups_.front(), change);
    if (item_notification_links_ != nullptr) {
        notify_reference_root_item_links(*item_notification_links_, context, change);
    }
}

void ReferenceProvider::add_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                                       std::uint32_t input_offset, std::size_t callback_data, std::uint32_t node_index,
                                       bool active, bool required, InputChangeFunction callback) {
    if (!subscriptions.has_subscriber_group()) {
        subscriber_groups_.push_back(nullptr);
        subscriptions.set_subscriber_group(static_cast<std::uint32_t>(subscriber_groups_.size() - 1));
    }

    auto &subscribers =
        DynamicSubscribers::ensure(subscriber_groups_[subscriptions.subscriber_group_index()], memory_);
    subscribers.add(graph, input_offset, callback_data, node_index, active, required, callback);
}

void ReferenceProvider::add_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, void *receiver,
                                     DynamicObserverFunction function) {
    if (!subscriptions.has_subscriber_group()) {
        subscriber_groups_.push_back(nullptr);
        subscriptions.set_subscriber_group(static_cast<std::uint32_t>(subscriber_groups_.size() - 1));
    }

    auto &subscribers =
        DynamicSubscribers::ensure(subscriber_groups_[subscriptions.subscriber_group_index()], memory_);
    subscribers.add_observer(graph, receiver, function);
}

void ReferenceProvider::remove_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                                          std::size_t input_offset) noexcept {
    if (subscriptions.has_subscriber_group()) {
        subscriber_groups_[subscriptions.subscriber_group_index()]->remove(graph, input_offset);
    }
}

void ReferenceProvider::remove_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                                        const void *receiver) noexcept {
    if (subscriptions.has_subscriber_group()) {
        subscriber_groups_[subscriptions.subscriber_group_index()]->remove_observer(graph, receiver);
    }
}

void ReferenceProvider::notify(ReferenceSubscriptionDetail &subscriptions, OutputChangeContext &context,
                               OutputChange change) {
    if (subscriptions.has_subscriber_group()) {
        context.notify(subscriber_groups_[subscriptions.subscriber_group_index()], change);
    }
    if (item_notification_links_ != nullptr) {
        notify_reference_item_links(*item_notification_links_, subscriptions, context, change);
    }
}

void ReferenceProvider::begin_item_notification_batch() noexcept {
    if (item_notification_links_ != nullptr) {
        begin_reference_item_notification_batch(*item_notification_links_);
    }
}

void ReferenceProvider::cancel_item_notification_batch() noexcept {
    if (item_notification_links_ != nullptr) {
        cancel_reference_item_notification_batch(*item_notification_links_);
    }
}

void ReferenceProvider::publish_item_notification_batch(OutputChangeContext &context) {
    if (item_notification_links_ != nullptr) {
        publish_reference_item_notification_batch(*item_notification_links_, context);
    }
}

void ReferenceProvider::add_item_notification_link(Reference reference, RuntimeGraph &graph, void *receiver,
                                                   item_id_t receiver_item_id,
                                                   ReferenceItemNotifyFunction notify_function,
                                                   ReferenceItemPublishFunction publish_function) {
    if (item_notification_links_ == nullptr) {
        item_notification_links_ = create_reference_item_notification_links(memory_);
    }
    if (reference.holds<RootReference>()) {
        add_reference_root_item_link(*item_notification_links_, graph, receiver, receiver_item_id, notify_function,
                                     publish_function);
        return;
    }

    add_reference_item_link(*item_notification_links_, reference_subscriptions(reference), graph, receiver,
                            receiver_item_id, notify_function, publish_function);
}

void ReferenceProvider::remove_item_notification_link(Reference reference, RuntimeGraph &graph, const void *receiver,
                                                      item_id_t receiver_item_id,
                                                      ReferenceItemNotifyFunction notify_function) noexcept {
    if (item_notification_links_ == nullptr) {
        return;
    }

    if (reference.holds<RootReference>()) {
        remove_reference_root_item_link(*item_notification_links_, graph, receiver, receiver_item_id, notify_function);
        return;
    }

    remove_reference_item_link(*item_notification_links_, reference_subscriptions(reference), graph, receiver,
                               receiver_item_id, notify_function);
}

void ReferenceProvider::add_cycle_callback(void *receiver, void (*function)(void *) noexcept) {
    owner().add_cycle_callback(receiver, function);
}

void ReferenceProvider::remove_cycle_callback(const void *receiver) noexcept {
    owner().remove_cycle_callback(receiver);
}

bool ReferenceProvider::release_item_reference(ItemReference &reference) noexcept {
    const auto release_subscribers = [&](ReferenceSubscriptionDetail &subscriptions) {
        if (!subscriptions.has_subscriber_group()) {
            return true;
        }

        auto &subscribers = subscriber_groups_[subscriptions.subscriber_group_index()];
        if (subscribers != nullptr && !subscribers->empty()) {
            return false;
        }

        DynamicSubscribers::release(subscribers);
        subscribers = nullptr;
        subscriptions.clear_subscriber_group();
        return true;
    };
    const auto retired = Reference{&reference};
    const auto depends_on_retired_reference = [&](Reference candidate) {
        while (candidate) {
            if (candidate == retired) {
                return true;
            }
            if (const auto *item = candidate.get<ItemReference>()) {
                candidate = item->parent();
                continue;
            }
            if (const auto *representation = candidate.get<RepresentationReference>()) {
                candidate = representation->base();
                continue;
            }
            if (candidate.holds<RootReference>()) {
                return false;
            }
        }
        return false;
    };

    for (auto index = representations_.size(); index != 0; --index) {
        auto *&representation = representations_[index - 1];
        if (representation == nullptr || !depends_on_retired_reference(representation->base_reference())) {
            continue;
        }

        auto &representation_reference = *representation->reference().get<RepresentationReference>();
        if (!release_subscribers(representation_reference.subscriptions())) {
            return false;
        }
        remove_reference_observer(representation->base_reference(), owner(), representation);
        representation->destroy(memory_);
        representation = nullptr;
    }

    return (item_notification_links_ == nullptr ||
            reference_item_links_empty(*item_notification_links_, reference.subscriptions())) &&
           release_subscribers(reference.subscriptions());
}

void ReferenceProvider::add_component(ReferenceProviderComponent &component) noexcept {
    component.next_ = components_;
    components_ = &component;
}

Reference ReferenceProvider::representation(Reference base_reference, const TimeSeriesType &requested_type) {
    const auto base_time_series = resolve_reference(base_reference);
    if (base_time_series.runtime_type->satisfies_reference_request(requested_type)) {
        return base_reference;
    }
    const auto root_time_series = resolve_reference(Reference{root_});
    if (root_time_series.runtime_type->satisfies_reference_request(requested_type)) {
        return Reference{root_};
    }

    for (auto *representation : representations_) {
        if (representation == nullptr) {
            continue;
        }
        if (representation->base_reference() == base_reference &&
            representation->resolve().runtime_type->satisfies_reference_request(requested_type)) {
            return representation->reference();
        }
    }

    auto *result =
        base_time_series.runtime_type->create_reference_representation(*this, base_reference, requested_type, memory_);
    auto release = util::scope_exit{[&]() noexcept { result->destroy(memory_); }};
    result->bind(static_cast<std::uint32_t>(representations_.size()));
    representations_.push_back(result);
    auto remove_entry = util::scope_exit{[&]() noexcept { representations_.pop_back(); }};
    add_reference_observer(base_reference, owner(), result,
                           [](OutputChangeContext &context, void *receiver, OutputChange change) {
                               static_cast<ReferenceRepresentation *>(receiver)->notify(context, change);
                           });
    remove_entry.release();
    release.release();
    return result->reference();
}

namespace {

RootReference &reference_root(Reference reference) noexcept {
    while (!reference.holds<RootReference>()) {
        reference = parent_reference(reference);
    }
    return *reference.get<RootReference>();
}

ReferenceProvider &require_provider(RootReference &root) {
    if (auto *provider = root.subscribers_or_provider.get<ReferenceProvider>()) {
        return *provider;
    }

    auto *subscribers = root.subscribers_or_provider.get<DynamicSubscribers>();
    auto &memory = root.owner_graph->allocation();
    auto *provider = memory.new_object<ReferenceProvider>(memory, root, subscribers);
    root.subscribers_or_provider = provider;
    return *provider;
}

ReferenceProvider &provider_for(Reference reference) noexcept {
    return *reference_root(reference).subscribers_or_provider.get<ReferenceProvider>();
}

} // namespace

ResolvedTimeSeries resolve_reference(Reference reference) {
    if (auto *root = reference.get<RootReference>()) {
        const auto node = root->owner_graph->node(*root->reference_node);
        return {&node.input_last_updated(0), node.input_value(0), root->referenced_runtime_type};
    }
    if (const auto *representation = reference.get<RepresentationReference>()) {
        return provider_for(reference).resolve_representation(representation->representation_index());
    }

    const auto &item_reference = *reference.get<ItemReference>();
    const auto parent = resolve_reference(item_reference.parent());
    const auto item = parent.runtime_type->item(parent.storage, item_reference.item_id());
    return {&item.last_updated, item.value, &item.runtime_type};
}

Reference reference_representation(Reference reference, const TimeSeriesType &requested_type) {
    const auto resolved = resolve_reference(reference);
    if (&resolved.runtime_type->type() == &requested_type) {
        return reference;
    }
    return require_reference_provider(reference).representation(reference, requested_type);
}

ReferenceProvider &require_reference_provider(Reference reference) {
    return require_provider(reference_root(reference));
}

void add_reference_subscriber(Reference reference, RuntimeGraph &graph, std::uint32_t input_offset,
                              std::size_t callback_data, std::uint32_t node_index, bool active, bool required,
                              InputChangeFunction callback) {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->subscribers_or_provider.get<ReferenceProvider>()) {
            provider->add_root_subscriber(graph, input_offset, callback_data, node_index, active, required, callback);
            return;
        }
        auto *subscribers = root->subscribers_or_provider.get<DynamicSubscribers>();
        auto &result = DynamicSubscribers::ensure(subscribers, root->owner_graph->allocation());
        root->subscribers_or_provider = subscribers;
        result.add(graph, input_offset, callback_data, node_index, active, required, callback);
        return;
    }
    provider_for(reference).add_subscriber(reference_subscriptions(reference), graph, input_offset, callback_data,
                                           node_index, active, required, callback);
}

void add_reference_observer(Reference reference, RuntimeGraph &graph, void *receiver,
                            DynamicObserverFunction function) {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->subscribers_or_provider.get<ReferenceProvider>()) {
            provider->add_root_observer(graph, receiver, function);
            return;
        }
        auto *subscribers = root->subscribers_or_provider.get<DynamicSubscribers>();
        auto &result = DynamicSubscribers::ensure(subscribers, root->owner_graph->allocation());
        root->subscribers_or_provider = subscribers;
        result.add_observer(graph, receiver, function);
        return;
    }
    provider_for(reference).add_observer(reference_subscriptions(reference), graph, receiver, function);
}

void remove_reference_subscriber(Reference reference, RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->subscribers_or_provider.get<ReferenceProvider>()) {
            provider->remove_root_subscriber(graph, input_offset);
        } else if (auto *subscribers = root->subscribers_or_provider.get<DynamicSubscribers>()) {
            subscribers->remove(graph, input_offset);
        }
        return;
    }
    provider_for(reference).remove_subscriber(reference_subscriptions(reference), graph, input_offset);
}

void remove_reference_observer(Reference reference, RuntimeGraph &graph, const void *receiver) noexcept {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->subscribers_or_provider.get<ReferenceProvider>()) {
            provider->remove_root_observer(graph, receiver);
        } else if (auto *subscribers = root->subscribers_or_provider.get<DynamicSubscribers>()) {
            subscribers->remove_observer(graph, receiver);
        }
        return;
    }
    provider_for(reference).remove_observer(reference_subscriptions(reference), graph, receiver);
}

void notify_reference(Reference reference, OutputChangeContext &context, OutputChange change) {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->subscribers_or_provider.get<ReferenceProvider>()) {
            provider->notify_root(context, change);
        } else {
            context.notify(root->subscribers_or_provider.get<DynamicSubscribers>(), change);
        }
        return;
    }
    provider_for(reference).notify(reference_subscriptions(reference), context, change);
}

void add_reference_item_notification_link(Reference reference, RuntimeGraph &graph, void *receiver,
                                          item_id_t receiver_item_id, ReferenceItemNotifyFunction notify_function,
                                          ReferenceItemPublishFunction publish_function) {
    auto &provider = require_reference_provider(reference);
    provider.add_item_notification_link(reference, graph, receiver, receiver_item_id, notify_function,
                                        publish_function);
}

void remove_reference_item_notification_link(Reference reference, RuntimeGraph &graph, const void *receiver,
                                             item_id_t receiver_item_id,
                                             ReferenceItemNotifyFunction notify_function) noexcept {
    if (!reference) {
        return;
    }
    auto *root = reference_root(reference).subscribers_or_provider.get<ReferenceProvider>();
    if (root == nullptr) {
        return;
    }
    root->remove_item_notification_link(reference, graph, receiver, receiver_item_id, notify_function);
}

void destroy_root_reference(RootReference &root, allocator &memory) noexcept {
    if (auto *provider = root.subscribers_or_provider.get<ReferenceProvider>()) {
        allocator{memory}.delete_object(provider);
    } else {
        DynamicSubscribers::release(root.subscribers_or_provider.get<DynamicSubscribers>());
    }
    std::destroy_at(&root);
}

} // namespace bg::details
