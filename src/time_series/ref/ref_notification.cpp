#include <bg/time_series/ref/ref_notification.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/util/scope_exit.hpp>

namespace bg {

void ReferenceInputStorage::replace_deferred(DeferredReferenceInputState &state,
                                             bool selected_view_valid) noexcept {
    assert(has_deferred_state());
    auto *previous = deferred_state();
    const auto previous_reference = previous->reference;
    set_deferred(state, selected_view_valid);
    auto &memory = details::ReferenceProvider::root(previous_reference).owner_graph->allocation();
    memory.delete_object(previous);
}

void ReferenceInputStorage::release_deferred(bool selected_view_valid) noexcept {
    if (!has_deferred_state()) {
        return;
    }

    auto *state = deferred_state();
    const auto reference = state->reference;
    set(reference, selected_view_valid);
    auto &memory = details::ReferenceProvider::root(reference).owner_graph->allocation();
    memory.delete_object(state);
}

} // namespace bg

namespace bg::details {

void ReferenceInputSubscriber::update_input(OutputChangeContext &context, ReferenceInputSampleLookup &samples,
                                            Reference new_reference,
                                            std::size_t notification_data,
                                            InputNotificationFunction notification) const {
    if (new_reference) {
        new_reference = ReferenceProvider::representation(new_reference, requested_runtime_type());
    }

    auto &graph = context.runtime_graph();
    auto &input = *static_cast<ReferenceInputStorage *>(graph.storage_at(input_offset()));
    const auto previous_reference = input.reference();
    const auto previous = previous_reference ? ResolvedTimeSeries::resolve(previous_reference)
                                             : ResolvedTimeSeries{};
    auto previous_view_was_valid = false;
    if (previous.runtime_type != nullptr) {
        previous_view_was_valid = previous.last_updated.valid_before(context.evaluation_time());
    }
    const auto current = new_reference ? ResolvedTimeSeries::resolve(new_reference) : ResolvedTimeSeries{};
    const auto same_target = previous_reference == new_reference ||
                             (previous.runtime_type == current.runtime_type && previous.storage == current.storage);
    const auto target_updated = current.runtime_type != nullptr &&
                                current.last_updated.time() == context.evaluation_time();
    const auto dereferenced_change = !same_target || target_updated;
    const auto needs_subscription = active() || required() || notification != nullptr || controlled_activity();
    auto new_subscription_added = false;
    auto remove_new_subscription = util::scope_exit{[&]() noexcept {
        if (new_subscription_added) {
            ReferenceProvider::remove_subscriber(new_reference, graph, input_offset());
        }
    }};
    if (previous_reference != new_reference && new_reference && needs_subscription) {
        new_subscription_added = true;
        ReferenceProvider::add_subscriber(new_reference, graph, static_cast<std::uint32_t>(input_offset()),
                                          notification_data, static_cast<std::uint32_t>(node_index()), active(),
                                          required(), notification, controlled_activity());
    }

    const auto rebind_deferred_state = input.has_deferred_state() && same_target && !target_updated &&
                                       previous_reference != new_reference;
    auto *new_deferred = static_cast<DeferredReferenceInputState *>(nullptr);
    auto release_new_deferred = util::scope_exit{[&]() noexcept {
        if (new_deferred != nullptr) {
            auto &memory = ReferenceProvider::root(new_deferred->reference).owner_graph->allocation();
            memory.delete_object(new_deferred);
        }
    }};
    if (rebind_deferred_state) {
        const auto &previous_memory = ReferenceProvider::root(input.deferred_state()->reference)
                                          .owner_graph->allocation()
                                          .memory_resource();
        const auto &new_memory = ReferenceProvider::root(new_reference).owner_graph->allocation().memory_resource();
        if (&previous_memory != &new_memory) {
            auto &memory = ReferenceProvider::root(new_reference).owner_graph->allocation();
            new_deferred = memory.new_object<DeferredReferenceInputState>(
                DeferredReferenceInputState{new_reference, input.deferred_state()->last_updated});
        }
    }
    auto *sample_storage = sampled_reference()
                               ? static_cast<ReferenceInputSampleStorage *>(
                                     graph.storage_at(ReferenceInputSampleStorage::offset_for(
                                         input_offset(), controlled_activity())))
                               : nullptr;
    auto new_sample = SampledReferenceChange{};
    auto callback_added = false;
    auto remove_new_callback = util::scope_exit{[&]() noexcept {
        if (callback_added) {
            graph.remove_cycle_callback(sample_storage);
        }
    }};
    if (sampled_reference() && previous_reference != new_reference && !same_target) {
        const auto has_sample = !sample_storage->empty();
        auto previous = has_sample ? sample_storage->previous()
                                   : (previous_reference ? ResolvedTimeSeries::resolve(previous_reference)
                                                         : ResolvedTimeSeries{});
        const auto previous_was_valid = previous.runtime_type != nullptr
                                            ? previous.last_updated.valid_before(context.evaluation_time())
                                            : previous_view_was_valid;
        if (previous_was_valid && previous.runtime_type != nullptr && previous.storage != nullptr &&
            !previous.valid()) {
            previous.last_updated = TimeSeriesState::from_validity_transition(context.evaluation_time(),
                                                                              previous_was_valid, true);
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

    const auto *prior_state = context.prior_state();
    if (same_target && !target_updated && new_reference && !input.has_deferred_state() && prior_state != nullptr) {
        const auto state = input.observed_state(*prior_state, previous);
        auto &memory = ReferenceProvider::root(new_reference).owner_graph->allocation();
        new_deferred = memory.new_object<DeferredReferenceInputState>(
            DeferredReferenceInputState{new_reference, state});
    }

    if (input.has_deferred_state()) {
        if (!same_target || target_updated) {
            input.release_deferred(previous_view_was_valid);
        } else if (rebind_deferred_state) {
            if (new_deferred != nullptr) {
                input.replace_deferred(*new_deferred, previous_view_was_valid);
                release_new_deferred.release();
            } else {
                input.deferred_state()->reference = new_reference;
            }
        }
    }
    if (previous_reference != new_reference) {
        if (previous_reference && needs_subscription) {
            ReferenceProvider::remove_subscriber(previous_reference, graph, input_offset());
        }
        if (sampled_reference() && !same_target) {
            sample_storage->clear();
            if (new_sample) {
                sample_storage->set(new_sample);
            }
        }
    }
    if (new_deferred != nullptr) {
        if (!input.has_deferred_state()) {
            input.set_deferred(*new_deferred, previous_view_was_valid);
            release_new_deferred.release();
        }
    } else if (!input.has_deferred_state()) {
        input.set(new_reference, previous_view_was_valid);
    }
    remove_new_subscription.release();
    remove_new_callback.release();

    if (dereferenced_change) {
        auto dereferenced_output_change = OutputChange::update;
        if (same_target) {
            if (current.last_updated.became_invalid()) {
                dereferenced_output_change = OutputChange::invalidation;
            } else if (current.last_updated.became_valid()) {
                dereferenced_output_change = OutputChange::revalidation;
            }
        } else if (previous_view_was_valid && !current.valid()) {
            dereferenced_output_change = OutputChange::invalidation;
        } else if (!previous_view_was_valid && current.valid()) {
            dereferenced_output_change = OutputChange::revalidation;
        }

        graph.notify_dynamic_subscriber(
            DynamicNodeSubscriber{graph, static_cast<std::uint32_t>(input_offset()),
                                  static_cast<std::uint32_t>(node_index()), active(), required(),
                                  controlled_activity()},
            dereferenced_output_change);
        if (required() && !same_target) {
            context.schedule_validity_check(graph, node_index());
        }
        if (notification != nullptr) {
            notification(context, node_index(), notification_data, dereferenced_output_change);
        }
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
        subscriber.update_input(context, samples, reference, 0, nullptr);
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
                                               subscriber.notification_data(), subscriber.notification());
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
        subscriber.update_input(context, samples, reference, 0, nullptr);
    }
    for (const auto &subscriber : notifying_subscribers) {
        subscriber.subscription().update_input(context, samples, reference,
                                               subscriber.notification_data(),
                                               subscriber.notification());
    }
}

} // namespace bg::details
