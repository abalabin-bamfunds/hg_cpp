#include <bg/time_series/ref/ref_provider.hpp>

#include "ref_item_notifications.hpp"

#include <bg/allocator.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <concepts>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>

namespace bg::details {

ReferenceRepresentation::~ReferenceRepresentation() = default;
ReferenceProviderComponent::~ReferenceProviderComponent() = default;

ReferenceSubscriptionDetail &ReferenceProvider::subscriptions(Reference reference) noexcept {
    if (auto *item = reference.get<ItemReference>()) {
        return item->subscriptions();
    }
    return reference.get<RepresentationReference>()->subscriptions();
}

Reference ReferenceProvider::parent(Reference reference) noexcept {
    if (const auto *item = reference.get<ItemReference>()) {
        return item->parent();
    }
    if (const auto *representation = reference.get<RepresentationReference>()) {
        return representation->base();
    }
    return {};
}

RootReference &ReferenceProvider::root(Reference reference) noexcept {
    while (!reference.holds<RootReference>()) {
        reference = parent(reference);
    }
    return *reference.get<RootReference>();
}

ReferenceProvider *ReferenceProvider::get(Reference reference) noexcept {
    if (!reference) {
        return nullptr;
    }
    return root(reference).provider();
}

ReferenceProvider &ReferenceProvider::require(RootReference &root) {
    if (auto *provider = root.provider()) {
        return *provider;
    }

    auto *subscribers = root.dynamic_subscribers();
    if (root.has_inline_subscriber()) {
        subscribers = &root.promote_subscribers();
    }
    auto &memory = root.owner_graph->allocation();
    auto *provider = memory.new_object<ReferenceProvider>(memory, root, subscribers);
    root.set_provider(*provider);
    return *provider;
}

DynamicSubscribers &RootReference::promote_subscribers() {
    auto &memory = owner_graph->allocation();
    notifications_.promote(memory);
    auto *subscribers = dynamic_subscribers();
    assert(subscribers != nullptr);
    return *subscribers;
}

void RootReference::add_subscriber(DynamicNodeSubscriber subscriber) {
    notifications_.add(subscriber, owner_graph->allocation());
}

void RootReference::remove_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept {
    notifications_.remove(graph, input_offset);
}

void RootReference::release_subscribers() noexcept { notifications_.release(); }

ReferenceProvider &ReferenceProvider::require(Reference reference) {
    return require(root(reference));
}

ReferenceProvider::ReferenceProvider(allocator memory, RootReference &root, DynamicSubscribers *root_subscribers)
    : memory_{memory}, root_{&root}, representations_{&memory.memory_resource()},
      subscriber_groups_{&memory.memory_resource()} {
    subscriber_groups_.push_back(root_subscribers);
}

ReferenceProvider::~ReferenceProvider() {
    for (auto *component = components_; component != nullptr; component = component->next_component) {
        component->detach();
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        remove_observer(representation->base_reference(), owner(), representation);
    }
    for (auto *representation : representations_ | std::views::reverse) {
        if (representation == nullptr) {
            continue;
        }
        representation->destroy(memory_);
    }
    while (components_ != nullptr) {
        auto *component = components_;
        components_ = component->next_component;
        component->destroy(memory_);
    }
    for (auto *subscribers : subscriber_groups_) {
        DynamicSubscribers::release(subscribers);
    }
    memory_.delete_object(item_notifications_);
}

ResolvedTimeSeries ReferenceProvider::resolve_representation(std::uint32_t index) const {
    assert(representations_[index] != nullptr);
    return representations_[index]->resolve();
}

void ReferenceProvider::add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset,
                                            std::size_t notification_data, std::uint32_t node_index, bool active,
                                            bool required, InputNotificationFunction notification,
                                            bool controlled_activity) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), memory_);
    subscribers.add(graph, input_offset, notification_data, node_index, active, required, notification,
                    controlled_activity);
}

void ReferenceProvider::add_root_observer(RuntimeGraph &graph, void *receiver, RuntimeObserverFunction function) {
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
    context.notify_subscribers(subscriber_groups_.front(), change);
    if (item_notifications_ != nullptr) {
        item_notifications_->notify_root(context, change);
    }
}

void ReferenceProvider::add_subscriber(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph,
                                       std::uint32_t input_offset, std::size_t notification_data,
                                       std::uint32_t node_index, bool active, bool required,
                                       InputNotificationFunction notification, bool controlled_activity) {
    if (!subscriptions.has_subscriber_group()) {
        subscriber_groups_.push_back(nullptr);
        subscriptions.set_subscriber_group(static_cast<std::uint32_t>(subscriber_groups_.size() - 1));
    }

    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_[subscriptions.subscriber_group_index()], memory_);
    subscribers.add(graph, input_offset, notification_data, node_index, active, required, notification,
                    controlled_activity);
}

void ReferenceProvider::add_observer(ReferenceSubscriptionDetail &subscriptions, RuntimeGraph &graph, void *receiver,
                                     RuntimeObserverFunction function) {
    if (!subscriptions.has_subscriber_group()) {
        subscriber_groups_.push_back(nullptr);
        subscriptions.set_subscriber_group(static_cast<std::uint32_t>(subscriber_groups_.size() - 1));
    }

    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_[subscriptions.subscriber_group_index()], memory_);
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

void ReferenceProvider::notify_subscriptions(const ReferenceSubscriptionDetail &subscriptions,
                                             OutputChangeContext &context, OutputChange change) {
    if (subscriptions.has_subscriber_group()) {
        context.notify_subscribers(subscriber_groups_[subscriptions.subscriber_group_index()], change);
    }
    if (item_notifications_ != nullptr) {
        item_notifications_->notify(subscriptions, context, change);
    }
}

void ReferenceProvider::begin_item_notification_batch() noexcept {
    if (item_notifications_ != nullptr) {
        item_notifications_->begin_batch();
    }
}

void ReferenceProvider::cancel_item_notification_batch() noexcept {
    if (item_notifications_ != nullptr) {
        item_notifications_->cancel_batch();
    }
}

void ReferenceProvider::publish_item_notification_batch(OutputChangeContext &context) {
    if (item_notifications_ != nullptr) {
        item_notifications_->publish_batch(context);
    }
}

void ReferenceProvider::add_item_notification_record(Reference reference, RuntimeGraph &graph,
                                                     void *destination_representation, item_id_t destination_item_id,
                                                     ItemChangeNotificationFunction notify_item_change,
                                                     ItemChangePublicationFunction publish_item_changes) {
    if (item_notifications_ == nullptr) {
        item_notifications_ = memory_.new_object<ItemNotifications>(memory_);
    }
    if (reference.holds<RootReference>()) {
        item_notifications_->add_root(graph, destination_representation, destination_item_id, notify_item_change,
                                      publish_item_changes);
        return;
    }

    item_notifications_->add(subscriptions(reference), graph, destination_representation, destination_item_id,
                             notify_item_change, publish_item_changes);
}

void ReferenceProvider::remove_item_notification_record(Reference reference, RuntimeGraph &graph,
                                                        const void *destination_representation,
                                                        item_id_t destination_item_id,
                                                        ItemChangeNotificationFunction notify_item_change) noexcept {
    if (item_notifications_ == nullptr) {
        return;
    }

    if (reference.holds<RootReference>()) {
        item_notifications_->remove_root(graph, destination_representation, destination_item_id, notify_item_change);
        return;
    }

    item_notifications_->remove(subscriptions(reference), graph, destination_representation, destination_item_id,
                                notify_item_change);
}

void ReferenceProvider::add_cycle_callback(void *receiver,
                                           void (*function)(void *, EvaluationContext &) noexcept) {
    owner().add_cycle_callback(receiver, function);
}

void ReferenceProvider::remove_cycle_callback(const void *receiver) noexcept {
    owner().remove_cycle_callback(receiver);
}

bool ReferenceProvider::item_reference_has_subscribers(const ItemReference &reference) const noexcept {
    const auto &subscriptions = reference.subscriptions();
    if (!subscriptions.has_subscriber_group()) {
        return false;
    }

    const auto *subscribers = subscriber_groups_[subscriptions.subscriber_group_index()];
    return subscribers != nullptr && !subscribers->empty();
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
        remove_observer(representation->base_reference(), owner(), representation);
        representation->destroy(memory_);
        representation = nullptr;
    }

    const auto has_item_notifications =
        item_notifications_ != nullptr && item_notifications_->has_item_notifications(reference.subscriptions());
    return !has_item_notifications && release_subscribers(reference.subscriptions());
}

void ReferenceProvider::add_component(ReferenceProviderComponent &component) noexcept {
    component.next_component = components_;
    components_ = &component;
}

Reference ReferenceProvider::make_representation(Reference base_reference,
                                                 const TimeSeriesRuntimeType &requested_runtime_type) {
    const auto base_time_series = ResolvedTimeSeries::resolve(base_reference);
    if (&base_time_series.runtime_type->type() == &requested_runtime_type.type()) {
        return base_reference;
    }
    const auto root_time_series = ResolvedTimeSeries::resolve(Reference{root_});
    if (&root_time_series.runtime_type->type() == &requested_runtime_type.type()) {
        return Reference{root_};
    }

    for (auto *representation : representations_) {
        if (representation == nullptr) {
            continue;
        }
        if (representation->base_reference() == base_reference &&
            &representation->resolve().runtime_type->type() == &requested_runtime_type.type()) {
            return representation->reference();
        }
    }

    const auto *factory = base_time_series.runtime_type->reference_representation_factory();
    if (factory == nullptr) {
        throw std::invalid_argument{"the requested time series representation is not implemented"};
    }
    auto *result =
        factory->create(*this, base_reference, *base_time_series.runtime_type, requested_runtime_type, memory_);
    auto release = util::scope_exit{[&]() noexcept { result->destroy(memory_); }};
    result->initialize_reference(static_cast<std::uint32_t>(representations_.size()));
    representations_.push_back(result);
    auto remove_entry = util::scope_exit{[&]() noexcept { representations_.pop_back(); }};
    add_observer(base_reference, owner(), result,
                 [](OutputChangeContext &context, void *receiver, OutputChange change) {
                     static_cast<ReferenceRepresentation *>(receiver)->notify(context, change);
                 });
    remove_entry.release();
    release.release();
    return result->reference();
}

ResolvedTimeSeries ResolvedTimeSeries::resolve(Reference reference) {
    if (auto *root = reference.get<RootReference>()) {
        const auto node = root->owner_graph->node(root->node());
        if (root->references_output()) {
            return {&node.output_last_updated(), node.output_value(), root->referenced_runtime_type};
        }
        const auto input = node.input_data(0);
        return {input.last_updated, input.value, root->referenced_runtime_type};
    }
    if (const auto *representation = reference.get<RepresentationReference>()) {
        return ReferenceProvider::get(reference)->resolve_representation(representation->representation_index());
    }

    const auto &item_reference = *reference.get<ItemReference>();
    const auto parent_time_series = ResolvedTimeSeries::resolve(item_reference.parent());
    const auto item = parent_time_series.runtime_type->item(parent_time_series.storage, item_reference.item_id());
    return {&item.last_updated, item.value, &item.runtime_type};
}

Reference ReferenceProvider::representation(Reference reference, const TimeSeriesRuntimeType &requested_runtime_type) {
    const auto resolved = ResolvedTimeSeries::resolve(reference);
    if (!representation_required(*resolved.runtime_type, requested_runtime_type)) {
        return reference;
    }
    const auto &runtime_type = representation_runtime_type(*resolved.runtime_type, requested_runtime_type);
    return require(reference).make_representation(reference, runtime_type);
}

bool ReferenceProvider::representation_required(const TimeSeriesRuntimeType &provided,
                                                const TimeSeriesRuntimeType &requested) noexcept {
    if (&provided.type() == &requested.type()) {
        return false;
    }
    const auto *factory = requested.reference_representation_factory();
    return factory != nullptr && factory->is_required(provided, requested);
}

const TimeSeriesRuntimeType &ReferenceProvider::representation_runtime_type(const TimeSeriesRuntimeType &provided,
                                                                            const TimeSeriesRuntimeType &requested) {
    const auto *factory = requested.reference_representation_factory();
    if (factory == nullptr) {
        throw std::invalid_argument{"the requested time series representation is not implemented"};
    }
    return factory->representation_runtime_type(provided, requested);
}

void ReferenceProvider::add_subscriber(Reference reference, RuntimeGraph &graph, std::uint32_t input_offset,
                                       std::size_t notification_data, std::uint32_t node_index, bool active,
                                       bool required, InputNotificationFunction notification,
                                       bool controlled_activity) {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->provider()) {
            provider->add_root_subscriber(graph, input_offset, notification_data, node_index, active, required,
                                          notification, controlled_activity);
            return;
        }

        const auto has_node_subscriber = active || required || controlled_activity;
        if (!has_node_subscriber && notification == nullptr) {
            return;
        }
        if (notification == nullptr) {
            root->add_subscriber(
                DynamicNodeSubscriber{graph, input_offset, node_index, active, required, controlled_activity});
            return;
        }

        root->promote_subscribers().add(graph, input_offset, notification_data, node_index, active, required,
                                        notification, controlled_activity);
        return;
    }
    get(reference)->add_subscriber(subscriptions(reference), graph, input_offset, notification_data, node_index, active,
                                   required, notification, controlled_activity);
}

void ReferenceProvider::add_observer(Reference reference, RuntimeGraph &graph, void *receiver,
                                     RuntimeObserverFunction function) {
    if (reference.holds<RootReference>()) {
        add_root_observer(graph, receiver, function);
        return;
    }
    add_observer(subscriptions(reference), graph, receiver, function);
}

void ReferenceProvider::remove_subscriber(Reference reference, RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (auto *root = reference.get<RootReference>()) {
        if (auto *provider = root->provider()) {
            provider->remove_root_subscriber(graph, input_offset);
        } else {
            root->remove_subscriber(graph, input_offset);
        }
        return;
    }
    get(reference)->remove_subscriber(subscriptions(reference), graph, input_offset);
}

void ReferenceProvider::remove_observer(Reference reference, RuntimeGraph &graph, const void *receiver) noexcept {
    if (reference.holds<RootReference>()) {
        remove_root_observer(graph, receiver);
        return;
    }
    remove_observer(subscriptions(reference), graph, receiver);
}

void ReferenceProvider::notify(Reference reference, OutputChangeContext &context, OutputChange change) {
    if (auto *root = reference.get<RootReference>()) {
        const auto destination = root->notification_destination();
        if (destination) {
            destination.visit([&]<typename T>(T *target) {
                if constexpr (std::same_as<T, DynamicSubscribers>) {
                    context.notify_subscribers(target, change);
                } else if constexpr (std::same_as<T, ReferenceProvider>) {
                    target->notify_root(context, change);
                } else {
                    target->notify_dynamic_subscriber(root->inline_subscriber(), change);
                }
            });
        }
        return;
    }
    get(reference)->notify_subscriptions(subscriptions(reference), context, change);
}

void ReferenceProvider::notify_item_bindings(Reference collection, item_id_t id,
                                             EvaluationContext &evaluation_context) {
    auto *provider = get(collection);
    if (provider == nullptr) {
        return;
    }

    auto context = OutputChangeContext{evaluation_context};
    for (auto *component = provider->components_; component != nullptr; component = component->next_component) {
        component->notify_item_bindings(collection, id, context);
    }
}

void ReferenceProvider::add_item_notification(Reference reference, RuntimeGraph &graph,
                                              void *destination_representation, item_id_t destination_item_id,
                                              ItemChangeNotificationFunction notify_item_change,
                                              ItemChangePublicationFunction publish_item_changes) {
    require(reference).add_item_notification_record(reference, graph, destination_representation, destination_item_id,
                                                    notify_item_change, publish_item_changes);
}

void ReferenceProvider::remove_item_notification(Reference reference, RuntimeGraph &graph,
                                                 const void *destination_representation, item_id_t destination_item_id,
                                                 ItemChangeNotificationFunction notify_item_change) noexcept {
    if (!reference) {
        return;
    }
    auto *provider = get(reference);
    if (provider == nullptr) {
        return;
    }
    provider->remove_item_notification_record(reference, graph, destination_representation, destination_item_id,
                                              notify_item_change);
}

void ReferenceProvider::destroy_root(RootReference &root, allocator &memory) noexcept {
    finalize_root(root, memory);
    std::destroy_at(&root);
}

void ReferenceProvider::finalize_root(RootReference &root, allocator &memory) noexcept {
    if (auto *provider = root.provider()) {
        allocator{memory}.delete_object(provider);
    } else {
        root.release_subscribers();
    }
    root.clear_notifications();
}

} // namespace bg::details
