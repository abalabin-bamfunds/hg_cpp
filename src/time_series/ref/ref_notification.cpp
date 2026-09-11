#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

namespace bg::details {

namespace {

void update_reference_input(OutputChangeContext &context, Reference new_reference,
                            const ReferenceInputSubscriber &subscriber, std::size_t callback_data,
                            InputChangeFunction callback, OutputChange change) {
    if (new_reference) {
        new_reference = reference_representation(new_reference, subscriber.requested_type());
    }

    auto &input = *static_cast<ReferenceInputStorage *>(context.runtime_graph().storage_at(subscriber.input_offset()));
    if (input.value != new_reference) {
        const auto needs_subscription = subscriber.active() || subscriber.required() || callback != nullptr;
        if (input.value && needs_subscription) {
            remove_reference_subscriber(input.value, context.runtime_graph(), subscriber.input_offset());
        }
        if (new_reference && needs_subscription) {
            add_reference_subscriber(new_reference, context.runtime_graph(),
                                     static_cast<std::uint32_t>(subscriber.input_offset()), callback_data,
                                     static_cast<std::uint32_t>(subscriber.node_index()), subscriber.active(),
                                     subscriber.required(), callback);
        }
        input.value = new_reference;
    }

    if (subscriber.required()) {
        context.schedule_validity_check(context.runtime_graph(), subscriber.node_index());
    }
    if (callback != nullptr) {
        callback(context, subscriber.node_index(), callback_data, change);
    }
}

} // namespace

void notify_reference_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type, const void *value,
                             OutputChange change, std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const ReferenceInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ReferenceInputSubscriber),
    };
    const auto reference = change == OutputChange::invalidation ? Reference{}
                           : runtime_type == nullptr            ? *static_cast<const Reference *>(value)
                                                                : runtime_type->reference(value);

    for (const auto &subscriber : records) {
        update_reference_input(context, reference, subscriber, 0, nullptr, change);
    }
}

void notify_callback_reference_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type,
                                      const void *value, OutputChange change, std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const CallbackReferenceInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(CallbackReferenceInputSubscriber),
    };
    const auto reference = change == OutputChange::invalidation ? Reference{}
                           : runtime_type == nullptr            ? *static_cast<const Reference *>(value)
                                                                : runtime_type->reference(value);

    for (const auto &subscriber : records) {
        update_reference_input(context, reference, subscriber.subscription(), subscriber.callback_data(),
                               subscriber.callback(), change);
    }
}

} // namespace bg::details
