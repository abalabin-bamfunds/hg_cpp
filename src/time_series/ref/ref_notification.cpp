#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

namespace bg::details {

namespace {

void update_reference_input(OutputNotificationContext &context, ReferenceAddress reference,
                            const ReferenceInputSubscriber &subscriber, std::size_t data,
                            InputNotificationFunction callback, OutputChange change) {
    if (reference) {
        reference = reference_representation(reference, subscriber.requested_type());
    }

    auto &input = *static_cast<ReferenceInputStorage *>(context.runtime_storage_at(subscriber.input_offset()));
    if (input.target != reference) {
        const auto subscribes_to_target = subscriber.active() || subscriber.required() || callback != nullptr;
        if (input.target && subscribes_to_target) {
            remove_reference_subscriber(input.target, context.runtime_graph(), subscriber.input_offset());
        }
        if (reference && subscribes_to_target) {
            add_reference_subscriber(reference, context.runtime_graph(),
                                     static_cast<std::uint32_t>(subscriber.input_offset()), data,
                                    static_cast<std::uint32_t>(subscriber.node_index()), subscriber.active(),
                                    subscriber.required(), callback);
        }
        input.target = reference;
    }

    if (subscriber.required()) {
        context.schedule_validity_check(context.runtime_graph(), subscriber.node_index());
    }
    if (callback != nullptr) {
        callback(context, subscriber.node_index(), data, change);
    }
}

} // namespace

void notify_reference_inputs(OutputNotificationContext &context, const void *value, OutputChange change,
                             std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const ReferenceInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ReferenceInputSubscriber),
    };
    const auto reference =
        change == OutputChange::invalidation ? ReferenceAddress{} : *static_cast<const ReferenceAddress *>(value);

    for (const auto &subscriber : records) {
        update_reference_input(context, reference, subscriber, 0, nullptr, change);
    }
}

void notify_callback_reference_inputs(OutputNotificationContext &context, const void *value, OutputChange change,
                                      std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const CallbackReferenceInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(CallbackReferenceInputSubscriber),
    };
    const auto reference =
        change == OutputChange::invalidation ? ReferenceAddress{} : *static_cast<const ReferenceAddress *>(value);

    for (const auto &subscriber : records) {
        update_reference_input(context, reference, subscriber.subscription(), subscriber.data(), subscriber.callback(),
                               change);
    }
}

} // namespace bg::details
