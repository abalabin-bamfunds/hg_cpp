#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/util/scope_exit.hpp>

#include "../tss/tss_reference_sample.hpp"

namespace bg::details {

void ReferenceInputSubscriber::update_input(OutputChangeContext &context, Reference new_reference,
                                            std::size_t notification_data, InputNotificationFunction notification,
                                            OutputChange change) const {
    if (new_reference) {
        new_reference = ReferenceProvider::representation(new_reference, requested_runtime_type());
    }

    auto &graph = context.runtime_graph();
    auto &input = *static_cast<ReferenceInputStorage *>(graph.storage_at(input_offset()));
    if (!sampled_reference()) {
        if (input.value != new_reference) {
            const auto needs_subscription = active() || required() || notification != nullptr || controlled_activity();
            if (input.value && needs_subscription) {
                ReferenceProvider::remove_subscriber(input.value, graph, input_offset());
            }
            if (new_reference && needs_subscription) {
                ReferenceProvider::add_subscriber(new_reference, graph, static_cast<std::uint32_t>(input_offset()),
                                                  notification_data, static_cast<std::uint32_t>(node_index()),
                                                  active(), required(), notification, controlled_activity());
            }
            input.value = new_reference;
        }
    } else {
        auto *sample_storage = static_cast<ReferenceInputSampleStorage *>(
            graph.storage_at(reference_input_sample_offset(input_offset(), controlled_activity())));
        auto *new_set_sample = static_cast<SampledSet *>(nullptr);
        auto destroy_new_sample = util::scope_exit{[&]() noexcept {
            if (new_set_sample != nullptr) {
                destroy_set_sample(new_set_sample);
            }
        }};
        auto callback_added = false;
        auto remove_new_callback = util::scope_exit{[&]() noexcept {
            if (callback_added) {
                graph.remove_cycle_callback(sample_storage);
            }
        }};
        const auto current = new_reference ? ResolvedTimeSeries::resolve(new_reference) : ResolvedTimeSeries{};
        if (input.value != new_reference) {
            const auto has_sample = !sample_storage->empty();
            auto previous = has_sample ? sample_storage->previous()
                                       : (input.value ? ResolvedTimeSeries::resolve(input.value) : ResolvedTimeSeries{});
            const auto previous_was_valid = has_sample ? previous.valid() : sample_storage->target_was_valid();
            auto previous_state = TimeSeriesState{};
            if (previous_was_valid && previous.runtime_type != nullptr && previous.storage != nullptr &&
                !previous.valid()) {
                previous_state.publish(context.evaluation_time());
                previous.last_updated = &previous_state;
            }
            new_set_sample = make_set_sample(previous, current, context.evaluation_time(), graph.allocation());
            if (new_set_sample != nullptr && sample_storage->empty()) {
                graph.add_cycle_callback(sample_storage, [](void *value, EvaluationContext &) noexcept {
                    static_cast<ReferenceInputSampleStorage *>(value)->clear();
                });
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
        if (input.value != new_reference && new_reference && needs_subscription) {
            ReferenceProvider::add_subscriber(new_reference, graph, static_cast<std::uint32_t>(input_offset()),
                                              notification_data, static_cast<std::uint32_t>(node_index()), active(),
                                              required(), notification, controlled_activity());
            new_subscription_added = true;
        }
        if (input.value != new_reference) {
            const auto previous_reference = input.value;
            if (previous_reference && needs_subscription) {
                ReferenceProvider::remove_subscriber(previous_reference, graph, input_offset());
            }
            input.value = new_reference;
            sample_storage->clear();
            if (new_set_sample != nullptr) {
                sample_storage->set(new_set_sample);
                new_set_sample = nullptr;
            }
            sample_storage->set_target_was_valid(current.valid());
        } else {
            sample_storage->set_target_was_valid(current.valid());
        }
        destroy_new_sample.release();
        remove_new_callback.release();
        remove_new_subscription.release();
    }

    if (required()) {
        context.schedule_validity_check(graph, node_index());
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
