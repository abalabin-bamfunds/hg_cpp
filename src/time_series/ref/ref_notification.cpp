#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

namespace bg::details {

void notify_reference_inputs(OutputNotificationContext &context, const void *value, OutputChange change,
                             std::span<const std::byte> record_bytes) {
    const auto records = std::span{
        reinterpret_cast<const ReferenceInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ReferenceInputSubscriber),
    };
    auto *reference = change == OutputChange::invalidation ? nullptr : *static_cast<ReferenceStorage *const *>(value);

    for (const auto &subscriber : records) {
        auto &input = *static_cast<ReferenceInputStorage *>(context.runtime_storage_at(subscriber.input_offset()));
        if (input.target != reference) {
            if (input.target != nullptr && (subscriber.active() || subscriber.required())) {
                input.target->subscribers->remove(context.runtime_graph(), subscriber.input_offset());
            }
            if (reference != nullptr && (subscriber.active() || subscriber.required())) {
                auto &subscribers =
                    DynamicSubscribers::ensure(reference->subscribers, context.memory_resource(*reference->owner));
                subscribers.add(context.runtime_graph(), static_cast<std::uint32_t>(subscriber.input_offset()),
                                static_cast<std::uint32_t>(subscriber.node_index()), subscriber.active(),
                                subscriber.required());
            }
            input.target = reference;
        }

        if (subscriber.required()) {
            context.schedule_validity_check(context.runtime_graph(), subscriber.node_index());
        }
    }
}

} // namespace bg::details
