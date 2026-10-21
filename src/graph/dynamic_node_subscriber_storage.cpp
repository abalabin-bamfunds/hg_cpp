#include <bg/graph/dynamic_node_subscriber_storage.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>

namespace bg::details {
class ReferenceProvider;
}

namespace bg::details {

template <typename... AdditionalDestinations>
void DynamicNodeSubscriberStorage<AdditionalDestinations...>::add(DynamicNodeSubscriber subscriber, allocator memory) {
    if (auto *subscribers = dynamic_subscribers()) {
        subscribers->add(subscriber);
        return;
    }

    if (has_inline_subscriber()) {
        const auto current = inline_subscriber();
        const auto current_identity = current.identity();
        const auto added_identity = subscriber.identity();
        if (current_identity.graph == added_identity.graph &&
            current_identity.input_offset == added_identity.input_offset) {
            return;
        }

        auto *subscribers = memory.new_object<DynamicSubscribers>(memory);
        auto release_subscribers = util::scope_exit{[&]() noexcept { DynamicSubscribers::release(subscribers); }};
        subscribers->add(current);
        subscribers->add(subscriber);
        set_dynamic_subscribers(subscribers);
        release_subscribers.release();
        return;
    }

    assert(!destination);
    set_inline_subscriber(subscriber);
}

template <typename... AdditionalDestinations>
void DynamicNodeSubscriberStorage<AdditionalDestinations...>::promote(allocator memory) {
    if (has_dynamic_subscribers()) {
        return;
    }

    assert(!destination || has_inline_subscriber());
    auto *subscribers = memory.new_object<DynamicSubscribers>(memory);
    auto release_subscribers = util::scope_exit{[&]() noexcept { DynamicSubscribers::release(subscribers); }};
    if (has_inline_subscriber()) {
        subscribers->add(inline_subscriber());
    }
    set_dynamic_subscribers(subscribers);
    release_subscribers.release();
}

template <typename... AdditionalDestinations>
void DynamicNodeSubscriberStorage<AdditionalDestinations...>::remove(RuntimeGraph &graph,
                                                                     std::size_t offset) noexcept {
    if (auto *subscribers = dynamic_subscribers()) {
        subscribers->remove(graph, offset);
        return;
    }

    if (auto *inline_graph = destination.template get<RuntimeGraph>()) {
        if (inline_graph == &graph && input_offset == static_cast<std::uint32_t>(offset)) {
            destination = nullptr;
        }
    }
}

template <typename... AdditionalDestinations>
bool DynamicNodeSubscriberStorage<AdditionalDestinations...>::empty() const noexcept {
    if (const auto *subscribers = dynamic_subscribers()) {
        return subscribers->empty();
    }
    return !destination;
}

template <typename... AdditionalDestinations>
void DynamicNodeSubscriberStorage<AdditionalDestinations...>::notify(OutputChangeContext &context,
                                                                       OutputChange change) const {
    if (auto *subscribers = dynamic_subscribers()) {
        context.notify_subscribers(subscribers, change);
        return;
    }

    if (has_inline_subscriber()) {
        const auto subscriber = inline_subscriber();
        subscriber.graph->notify_dynamic_subscriber(subscriber, change);
    }
}

template <typename... AdditionalDestinations>
void DynamicNodeSubscriberStorage<AdditionalDestinations...>::release() noexcept {
    DynamicSubscribers::release(dynamic_subscribers());
    destination = nullptr;
}

template struct DynamicNodeSubscriberStorage<>;
template struct DynamicNodeSubscriberStorage<ReferenceProvider>;

} // namespace bg::details
