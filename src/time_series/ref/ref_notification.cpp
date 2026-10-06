#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

namespace bg::details {

void ReferenceInputSubscriber::update_input(OutputChangeContext &context, Reference new_reference,
                                            std::size_t notification_data, InputNotificationFunction notification,
                                            OutputChange change) const {
    if (new_reference) {
        new_reference = ReferenceProvider::representation(new_reference, requested_runtime_type());
    }

    auto &input = *static_cast<ReferenceInputStorage *>(context.runtime_graph().storage_at(input_offset()));
    if (input.value != new_reference) {
        const auto needs_subscription =
            active() || required() || all_valid() || notification != nullptr || controlled_activity();
        if (input.value && needs_subscription) {
            ReferenceProvider::remove_subscriber(input.value, context.runtime_graph(), input_offset());
        }
        if (new_reference && needs_subscription) {
            ReferenceProvider::add_subscriber(new_reference, context.runtime_graph(),
                                              static_cast<std::uint32_t>(input_offset()), notification_data,
                                              static_cast<std::uint32_t>(node_index()), active(), required(),
                                              notification, controlled_activity(), all_valid());
        }
        input.value = new_reference;
    }

    if (required() || all_valid()) {
        context.schedule_validity_check(context.runtime_graph(), node_index());
    }
    if (notification != nullptr) {
        notification(context, node_index(), notification_data, change);
    }
}

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
        subscriber.update_input(context, reference, 0, nullptr, change);
    }
}

void notify_reference_inputs_with_notifications(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type,
                                                const void *value, OutputChange change,
                                                std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const ReferenceInputNotificationSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ReferenceInputNotificationSubscriber),
    };
    const auto reference = change == OutputChange::invalidation ? Reference{}
                           : runtime_type == nullptr            ? *static_cast<const Reference *>(value)
                                                                : runtime_type->reference(value);

    for (const auto &subscriber : records) {
        subscriber.subscription().update_input(context, reference, subscriber.notification_data(),
                                               subscriber.notification(), change);
    }
}

} // namespace bg::details
