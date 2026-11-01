#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_node_subscriber.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace bg {

class RuntimeGraph;

namespace details {

struct DynamicSubscribers;

// Additional destinations are stored here but dispatched by the owning object.
template <typename... AdditionalDestinations> struct DynamicNodeSubscriberStorage {
    using Destination = util::variant_ptr_with_alignment<alignof(void *), RuntimeGraph, DynamicSubscribers,
                                                         AdditionalDestinations...>;

    bool has_inline_subscriber() const noexcept { return destination.template holds<RuntimeGraph>(); }
    bool has_dynamic_subscribers() const noexcept { return destination.template holds<DynamicSubscribers>(); }

    DynamicNodeSubscriber inline_subscriber() const noexcept {
        assert(has_inline_subscriber());
        return DynamicNodeSubscriber{*destination.template get<RuntimeGraph>(), input_offset, node_and_flags};
    }

    DynamicSubscribers *dynamic_subscribers() const noexcept {
        return destination.template get<DynamicSubscribers>();
    }

    void set_inline_subscriber(DynamicNodeSubscriber subscriber) noexcept {
        assert(!destination);
        input_offset = subscriber.input_offset;
        node_and_flags = subscriber.node_and_flags;
        destination = subscriber.graph;
    }

    void set_dynamic_subscribers(DynamicSubscribers *subscribers) noexcept {
        assert(subscribers != nullptr);
        assert(!destination || has_inline_subscriber());
        destination = subscribers;
    }

    void add(DynamicNodeSubscriber subscriber, allocator memory);
    void promote(allocator memory);
    void remove(RuntimeGraph &graph, std::size_t input_offset) noexcept;
    bool empty() const noexcept;
    void notify(OutputChangeContext &context, OutputChange change) const;
    void release() noexcept;

    Destination destination;
    std::uint32_t input_offset{};
    std::uint32_t node_and_flags{};
};

static_assert(sizeof(DynamicNodeSubscriberStorage<>) == 2 * sizeof(void *));

} // namespace details
} // namespace bg
