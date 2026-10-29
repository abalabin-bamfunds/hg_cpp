#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/util/scope_exit.hpp>

namespace bg::details {

void ReferenceInputSubscriber::update_input(OutputChangeContext &context, ReferenceInputSampleLookup &samples,
                                            Reference new_reference,
                                            std::size_t notification_data, InputNotificationFunction notification,
                                            OutputChange change) const {
    if (new_reference) {
        new_reference = ReferenceProvider::representation(new_reference, requested_runtime_type());
    }

    auto &graph = context.runtime_graph();
    auto &input = *static_cast<ReferenceInputStorage *>(graph.storage_at(input_offset()));
    const auto previous_reference = input.reference();
    auto previous_view_was_valid = false;
    if (previous_reference) {
        const auto previous = ResolvedTimeSeries::resolve(previous_reference);
        if (previous.runtime_type != nullptr) {
            previous_view_was_valid = previous.last_updated.valid_before(context.evaluation_time());
        }
    }
    const auto current = new_reference ? ResolvedTimeSeries::resolve(new_reference) : ResolvedTimeSeries{};
    if (!sampled_reference()) {
        if (previous_reference != new_reference) {
            const auto needs_subscription = active() || required() || notification != nullptr || controlled_activity();
            if (previous_reference && needs_subscription) {
                ReferenceProvider::remove_subscriber(previous_reference, graph, input_offset());
            }
            if (new_reference && needs_subscription) {
                ReferenceProvider::add_subscriber(new_reference, graph, static_cast<std::uint32_t>(input_offset()),
                                                  notification_data, static_cast<std::uint32_t>(node_index()),
                                                  active(), required(), notification, controlled_activity());
            }
        }
    } else {
        auto *sample_storage = static_cast<ReferenceInputSampleStorage *>(
            graph.storage_at(ReferenceInputSampleStorage::offset_for(input_offset(), controlled_activity())));
        auto new_sample = SampledReferenceChange{};
        auto callback_added = false;
        auto remove_new_callback = util::scope_exit{[&]() noexcept {
            if (callback_added) {
                graph.remove_cycle_callback(sample_storage);
            }
        }};
        if (previous_reference != new_reference) {
            const auto has_sample = !sample_storage->empty();
            auto previous = has_sample ? sample_storage->previous()
                                       : (previous_reference ? ResolvedTimeSeries::resolve(previous_reference)
                                                             : ResolvedTimeSeries{});
            const auto previous_was_valid = previous.runtime_type != nullptr
                                                ? previous.last_updated.valid_before(context.evaluation_time())
                                                : previous_view_was_valid;
            auto previous_state = TimeSeriesState{};
            if (previous_was_valid && previous.runtime_type != nullptr && previous.storage != nullptr &&
                !previous.valid()) {
                previous_state.publish(context.evaluation_time());
                previous.last_updated = previous_state;
            }
            new_sample = samples.get(previous_reference, new_reference, previous, current,
                                     requested_runtime_type(), context.evaluation_time());
            if (new_sample && sample_storage->empty()) {
                const auto clear_sample = [](void *value, EvaluationContext &) noexcept {
                    static_cast<ReferenceInputSampleStorage *>(value)->clear();
                };
                graph.add_cycle_callback(sample_storage, clear_sample, true);
                callback_added = true;
            }
        }

        const auto needs_subscription = active() || required() || notification != nullptr || controlled_activity();
        auto new_subscription_added = false;
        auto remove_new_subscription = util::scope_exit{[&]() noexcept {
            if (new_subscription_added) {
                ReferenceProvider::remove_subscriber(new_reference, graph, input_offset());
            }
        }};
        if (previous_reference != new_reference && new_reference && needs_subscription) {
            ReferenceProvider::add_subscriber(new_reference, graph, static_cast<std::uint32_t>(input_offset()),
                                              notification_data, static_cast<std::uint32_t>(node_index()), active(),
                                              required(), notification, controlled_activity());
            new_subscription_added = true;
        }
        if (previous_reference != new_reference) {
            if (previous_reference && needs_subscription) {
                ReferenceProvider::remove_subscriber(previous_reference, graph, input_offset());
            }
            sample_storage->clear();
            if (new_sample) {
                sample_storage->set(new_sample);
            }
        }
        remove_new_callback.release();
        remove_new_subscription.release();
    }
    input.set(new_reference, previous_view_was_valid);

    if (required()) {
        context.schedule_validity_check(graph, node_index());
    }
    if (notification != nullptr) {
        notification(context, node_index(), notification_data, change);
    }
}

void notify_reference_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type, const void *value,
                             OutputChange change, std::span<const std::byte> record_bytes) {
    auto &publisher_graph = context.runtime_graph();
    const auto reference = change == OutputChange::invalidation ? Reference{}
                           : runtime_type == nullptr            ? *static_cast<const Reference *>(value)
                                                                : runtime_type->reference(value);
    auto samples = ReferenceInputSampleLookup{publisher_graph.allocation()};
    const auto subscribers = std::span{
        reinterpret_cast<const ReferenceInputSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ReferenceInputSubscriber)};

    for (const auto &subscriber : subscribers) {
        subscriber.update_input(context, samples, reference, 0, nullptr, change);
    }
}

void notify_reference_inputs_with_notifications(OutputChangeContext &context,
                                                const TimeSeriesRuntimeType *runtime_type, const void *value,
                                                OutputChange change, std::span<const std::byte> record_bytes) {
    auto &publisher_graph = context.runtime_graph();
    const auto reference = change == OutputChange::invalidation ? Reference{}
                           : runtime_type == nullptr            ? *static_cast<const Reference *>(value)
                                                                : runtime_type->reference(value);
    auto samples = ReferenceInputSampleLookup{publisher_graph.allocation()};
    const auto subscribers = std::span{
        reinterpret_cast<const ReferenceInputNotificationSubscriber *>(record_bytes.data()),
        record_bytes.size() / sizeof(ReferenceInputNotificationSubscriber)};

    for (const auto &subscriber : subscribers) {
        subscriber.subscription().update_input(context, samples, reference,
                                               subscriber.notification_data(), subscriber.notification(), change);
    }
}

void notify_mixed_reference_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type,
                                   const void *value, OutputChange change, std::span<const std::byte> record_bytes) {
    auto &publisher_graph = context.runtime_graph();
    const auto &header = *reinterpret_cast<const ReferenceInputNotificationHeader *>(record_bytes.data());
    const auto *subscriber_data = record_bytes.data() + sizeof(header);
    const auto subscriber_bytes = header.subscriber_count * sizeof(ReferenceInputSubscriber);
    const auto subscribers = std::span{
        reinterpret_cast<const ReferenceInputSubscriber *>(subscriber_data), header.subscriber_count};
    const auto notifying_subscribers = std::span{
        reinterpret_cast<const ReferenceInputNotificationSubscriber *>(subscriber_data + subscriber_bytes),
        header.notifying_subscriber_count};
    const auto reference = change == OutputChange::invalidation ? Reference{}
                           : runtime_type == nullptr            ? *static_cast<const Reference *>(value)
                                                                : runtime_type->reference(value);
    auto samples = ReferenceInputSampleLookup{publisher_graph.allocation()};

    for (const auto &subscriber : subscribers) {
        subscriber.update_input(context, samples, reference, 0, nullptr, change);
    }
    for (const auto &subscriber : notifying_subscribers) {
        subscriber.subscription().update_input(context, samples, reference,
                                               subscriber.notification_data(),
                                               subscriber.notification(), change);
    }
}

} // namespace bg::details
