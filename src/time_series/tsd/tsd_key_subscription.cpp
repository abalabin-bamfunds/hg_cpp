#include <bg/time_series/tsd/tsd_key_subscription.hpp>

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {

class TsdKeySubscriptions;
struct SubscribedTsd;

struct TsdKeySelection {
  public:
    bool queued_for_release() const noexcept { return (state_ & release_queue_index_mask) != 0; }
    std::uint32_t release_queue_index() const noexcept {
        assert(queued_for_release());
        return (state_ & release_queue_index_mask) - 1;
    }
    void set_release_queue_index(std::uint32_t index) noexcept {
        assert(index < release_queue_index_mask);
        state_ = (state_ & retained_flag) | (index + 1);
    }
    void clear_release_queue_index() noexcept { state_ &= retained_flag; }
    bool retained() const noexcept { return (state_ & retained_flag) != 0; }
    void set_retained(bool value) noexcept {
        state_ = (state_ & release_queue_index_mask) | (value ? retained_flag : 0);
    }

    SubscribedTsd *tsd{};
    union SelectionStorage {
        ItemReference *value_item_reference;
        DynamicSubscribers *reference_item_subscribers;

        SelectionStorage() : value_item_reference{} {}
    } selection;
    Reference selected_reference{};
    EvaluationTime selection_time{MIN_DT};
    item_id_t id{};

  private:
    static constexpr auto retained_flag = std::uint32_t{1} << 31;
    static constexpr auto release_queue_index_mask = ~retained_flag;
    std::uint32_t state_{};
};

using TsdKeySelectionMap = util::hash_dictionary<item_id_t, TsdKeySelection>;

struct SubscribedTsd {
    SubscribedTsd(TsdKeySubscriptions &owner, Reference reference, RetainedDictionaryKey retained) noexcept
        : owner{&owner}, reference{reference}, runtime_type{retained.runtime_type}, storage{retained.value} {}

    TsdKeySubscriptions *owner;
    Reference reference;
    const TimeSeriesDictionaryRuntimeType *runtime_type;
    void *storage;
    TsdKeySelectionMap keys;
    bool observing_reference{};

    bool stores_references() const noexcept {
        return runtime_type->item_runtime_type().is_reference();
    }
};

namespace {

const TimeSeriesReferenceType &resolved_tsd_item_reference_type(const TimeSeriesDictionaryRuntimeType &tsd) {
    const auto &dictionary_type = static_cast<const TimeSeriesDictionaryType &>(tsd.type());
    if (tsd.item_runtime_type().is_reference()) {
        return static_cast<const TimeSeriesReferenceType &>(dictionary_type.value_type());
    }

    return time_series_reference_type(dictionary_type.value_type(), *dictionary_type.guarantee().value());
}

const Reference &selected_tsd_key_reference(const void *value) noexcept;
void subscribe_tsd_key_selection(void *value, RuntimeGraph &graph, std::uint32_t input_offset,
                                 std::size_t notification_data, std::uint32_t node_index,
                                 InputNotificationFunction notification);
void unsubscribe_tsd_key_selection(void *value, RuntimeGraph &graph, std::size_t input_offset) noexcept;

const ReferenceBindingOperations tsd_key_selection_operations{
    &selected_tsd_key_reference,
    &subscribe_tsd_key_selection,
    &unsubscribe_tsd_key_selection,
};

} // namespace

class TsdKeySubscriptions final : public ReferenceProviderComponent {
  public:
    TsdKeySubscriptions(ReferenceProvider &provider, allocator memory)
        : provider_{&provider},
          subscribed_tsds_{&memory.memory_resource()},
          release_queue_{&memory.memory_resource()},
          memory_{memory} {}

    ~TsdKeySubscriptions() override {
        detach();
        for (auto *tsd : subscribed_tsds_) {
            release_tsd(*tsd);
        }
    }

    void detach() noexcept override {
        provider_->remove_cycle_callback(this);
        for (auto *tsd : subscribed_tsds_) {
            if (tsd->observing_reference) {
                provider_->remove_observer(tsd->reference, provider_->owner(), tsd);
                tsd->observing_reference = false;
            }
        }
    }

    void destroy(allocator &memory) noexcept override { memory.delete_object(this); }

    [[nodiscard]] TsdItemReferenceInput input(Reference reference, Scalar<> key) {
        const auto retained = retain_key(reference, key);
        if (retained.runtime_type == nullptr) {
            return {&MIN_DT, nullptr, {}, false};
        }
        auto release_retention = util::scope_exit{
            [&]() noexcept { retained.runtime_type->release_key(retained.value, retained.id, memory_); }};

        auto &tsd = require_tsd(reference, retained);
        auto &entry = require_entry(tsd, retained.id);
        if (!entry.retained()) {
            entry.set_retained(true);
            release_retention.release();
        }

        refresh_reference_value(tsd, retained.id, entry, provider_->owner().evaluation_time());
        queue_release(entry);

        const auto &runtime_type =
            time_series_reference_runtime_type(resolved_tsd_item_reference_type(*tsd.runtime_type));
        return {&entry.selection_time, &runtime_type, ReferenceBinding{&entry, tsd_key_selection_operations},
                entry.selection_time == provider_->owner().evaluation_time()};
    }

    void add_binding_subscriber(TsdKeySelection &entry, RuntimeGraph &graph, std::uint32_t input_offset,
                                std::size_t notification_data, std::uint32_t node_index,
                                InputNotificationFunction notification) {
        remove_from_release_queue(entry);
        if (entry.tsd->stores_references()) {
            auto &subscribers = DynamicSubscribers::ensure(entry.selection.reference_item_subscribers, memory_);
            subscribers.add(graph, input_offset, notification_data, node_index, false, false, notification);
        } else {
            ReferenceProvider::add_subscriber(Reference{entry.selection.value_item_reference}, graph, input_offset,
                                              notification_data, node_index, false, false, notification);
        }
    }

    void remove_binding_subscriber(TsdKeySelection &entry, RuntimeGraph &graph, std::size_t input_offset) noexcept {
        if (entry.tsd->stores_references()) {
            entry.selection.reference_item_subscribers->remove(graph, input_offset);
        } else {
            ReferenceProvider::remove_subscriber(Reference{entry.selection.value_item_reference}, graph,
                                                 input_offset);
        }
        if (!has_active_subscribers(entry)) {
            queue_release(entry);
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(const EvaluationTime &, const void *, void *,
                                                                     TimeSeriesRuntimeType::ItemIdFunction) const;

    RetainedDictionaryKey retain_key(Reference reference, Scalar<> key) {
        const auto resolved = ResolvedTimeSeries::resolve(reference);
        if (resolved.storage == nullptr) {
            return {};
        }

        const auto &runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*resolved.runtime_type);
        return runtime_type.retain_key(const_cast<void *>(resolved.storage), scalar_data(key), memory_);
    }

    TsdKeySelection &require_entry(SubscribedTsd &tsd, item_id_t id) {
        if (auto *entry = tsd.keys.find(id)) {
            return *entry;
        }

        if (release_queue_.capacity() <= entry_count_) {
            release_queue_.reserve(std::max<std::size_t>(release_queue_.capacity() * 2, 8));
        }

        auto *value_item_reference =
            tsd.stores_references() ? nullptr : memory_.new_object<ItemReference>(tsd.reference, id);
        auto release_slot_reference = util::scope_exit{[&]() noexcept {
            if (value_item_reference != nullptr) {
                memory_.delete_object(value_item_reference);
            }
        }};
        auto [entry, inserted] = tsd.keys.try_emplace(id, memory_);
        assert(inserted);

        entry->tsd = &tsd;
        entry->selection.value_item_reference = value_item_reference;
        entry->id = id;
        if (value_item_reference != nullptr) {
            release_slot_reference.release();
        }
        ++entry_count_;
        return *entry;
    }

    static bool refresh_reference_value(SubscribedTsd &tsd, item_id_t id, TsdKeySelection &entry,
                                        EvaluationTime evaluation_time) {
        const auto key_is_live = tsd.runtime_type->key_live(tsd.storage, id);
        if (!key_is_live) {
            if (!entry.selected_reference) {
                return false;
            }
            entry.selected_reference = {};
            entry.selection_time = MIN_DT;
            return true;
        }

        const auto item = tsd.runtime_type->item(tsd.storage, id);

        auto value = Reference{};
        if (item.last_updated > MIN_DT) {
            value = tsd.stores_references() ? *static_cast<const Reference *>(item.value)
                                            : Reference{entry.selection.value_item_reference};
        }
        if (value == entry.selected_reference) {
            return false;
        }

        entry.selected_reference = value;
        entry.selection_time = value ? evaluation_time : MIN_DT;
        return true;
    }

    SubscribedTsd *find_tsd(Reference reference, RetainedDictionaryKey retained) const noexcept {
        const auto found = std::ranges::find_if(subscribed_tsds_, [&](const auto *candidate) {
            return candidate->reference == reference && candidate->runtime_type == retained.runtime_type &&
                   candidate->storage == retained.value;
        });
        return found == subscribed_tsds_.end() ? nullptr : *found;
    }

    SubscribedTsd &require_tsd(Reference reference, RetainedDictionaryKey retained) {
        if (auto *existing = find_tsd(reference, retained)) {
            return *existing;
        }

        auto *result = memory_.new_object<SubscribedTsd>(*this, reference, retained);
        auto release = util::scope_exit{[&]() noexcept { release_tsd(*result); }};
        subscribed_tsds_.push_back(result);
        auto remove_entry = util::scope_exit{[&]() noexcept { subscribed_tsds_.pop_back(); }};
        provider_->add_observer(reference, provider_->owner(), result, &notify_tsd);
        result->observing_reference = true;

        remove_entry.release();
        release.release();
        return *result;
    }

    void queue_release(TsdKeySelection &entry) noexcept {
        if (has_active_subscribers(entry) || entry.queued_for_release()) {
            return;
        }
        release_queue_.push_back(&entry);
        entry.set_release_queue_index(static_cast<std::uint32_t>(release_queue_.size() - 1));
        if (!cycle_callback_registered_) {
            provider_->add_cycle_callback(this, &release_queued_entries);
            cycle_callback_registered_ = true;
        }
    }

    void remove_from_release_queue(TsdKeySelection &entry) noexcept {
        if (!entry.queued_for_release()) {
            return;
        }
        const auto index = entry.release_queue_index();
        auto replacement = release_queue_.back();
        release_queue_[index] = replacement;
        replacement->set_release_queue_index(index);
        release_queue_.pop_back();
        entry.clear_release_queue_index();
    }

    void erase_key(TsdKeySelection &entry, item_id_t id) noexcept {
        auto &tsd = *entry.tsd;
        assert(!has_active_subscribers(entry));
        if (entry.retained()) {
            tsd.runtime_type->release_key(tsd.storage, id, memory_);
            entry.set_retained(false);
        }
        if (!tsd.stores_references()) {
            if (!provider_->release_item_reference(*entry.selection.value_item_reference)) {
                queue_release(entry);
                return;
            }
            memory_.delete_object(entry.selection.value_item_reference);
        } else {
            DynamicSubscribers::release(entry.selection.reference_item_subscribers);
        }
        tsd.keys.erase_with(id, memory_, [](item_id_t *) noexcept {},
                            [](TsdKeySelection *value) noexcept { std::destroy_at(value); });
        --entry_count_;
    }

    void erase_tsd(SubscribedTsd &tsd) noexcept {
        const auto position = std::ranges::find(subscribed_tsds_, &tsd);
        *position = subscribed_tsds_.back();
        subscribed_tsds_.pop_back();
        release_tsd(tsd);
    }

    void release_tsd(SubscribedTsd &tsd) noexcept {
        if (tsd.observing_reference) {
            provider_->remove_observer(tsd.reference, provider_->owner(), &tsd);
        }
        for (const auto entry : tsd.keys) {
            if (entry.value.retained()) {
                tsd.runtime_type->release_key(tsd.storage, entry.key, memory_);
            }
            if (!tsd.stores_references()) {
                memory_.delete_object(entry.value.selection.value_item_reference);
            } else {
                DynamicSubscribers::release(entry.value.selection.reference_item_subscribers);
            }
        }
        tsd.keys.release_with(memory_, [](item_id_t *) noexcept {},
                              [](TsdKeySelection *entry) noexcept { std::destroy_at(entry); });
        memory_.delete_object(&tsd);
    }

    static void notify_tsd(OutputChangeContext &context, void *value, OutputChange change) {
        auto &tsd = *static_cast<SubscribedTsd *>(value);
        tsd.owner->notify(context, tsd, change);
    }

    void notify(OutputChangeContext &context, SubscribedTsd &tsd, OutputChange change) {
        if (change == OutputChange::invalidation) {
            invalidate_all(context, tsd);
            release_retained_keys(tsd);
            return;
        }

        const auto resolved = ResolvedTimeSeries::resolve(tsd.reference);
        if (resolved.storage == nullptr) {
            invalidate_all(context, tsd);
            return;
        }
        const auto &runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*resolved.runtime_type);
        if (&runtime_type != tsd.runtime_type || resolved.storage != tsd.storage) {
            invalidate_all(context, tsd);
            return;
        }
        if (change != OutputChange::update) {
            notify_present(context, tsd, resolved, runtime_type, change);
            return;
        }

        notify_changes(context, tsd, resolved, runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_added,
                       OutputChange::revalidation);
        notify_changes(context, tsd, resolved, runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_removed,
                       OutputChange::invalidation);
        notify_changes(context, tsd, resolved, runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_modified,
                       OutputChange::update);
    }

    static void notify_selection(OutputChangeContext &context, TsdKeySelection &entry, OutputChange change) {
        if (!entry.tsd->stores_references()) {
            ReferenceProvider::notify(Reference{entry.selection.value_item_reference}, context, change);
        } else {
            context.notify_subscribers(entry.selection.reference_item_subscribers, change);
        }
    }

    static void invalidate_all(OutputChangeContext &context, SubscribedTsd &tsd) {
        for (const auto entry : tsd.keys) {
            const auto was_valid = static_cast<bool>(entry.value.selected_reference);
            entry.value.selected_reference = {};
            entry.value.selection_time = MIN_DT;
            if (was_valid) {
                notify_selection(context, entry.value, OutputChange::invalidation);
            }
        }
    }

    void release_retained_keys(SubscribedTsd &tsd) noexcept {
        for (const auto entry : tsd.keys) {
            if (!entry.value.retained()) {
                continue;
            }
            tsd.runtime_type->release_key(tsd.storage, entry.key, memory_);
            entry.value.set_retained(false);
        }
    }

    static void notify_present(OutputChangeContext &context, SubscribedTsd &tsd, ResolvedTimeSeries resolved,
                               const TimeSeriesDictionaryRuntimeType &runtime_type, OutputChange change) {
        for (const auto entry : tsd.keys) {
            if (runtime_type.key_live(resolved.storage, entry.key)) {
                notify_item(context, tsd, entry.key, change);
            }
        }
    }

    void notify_changes(OutputChangeContext &context, SubscribedTsd &tsd, ResolvedTimeSeries resolved,
                        const TimeSeriesDictionaryRuntimeType &runtime_type, ChangeFunction changes,
                        OutputChange change) {
        struct Callback {
            TsdKeySubscriptions *owner;
            SubscribedTsd *tsd;
            OutputChangeContext *context;
            OutputChange change;
        } callback{this, &tsd, &context, change};

        (runtime_type.*changes)(*resolved.last_updated, resolved.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            callback.owner->notify_item(*callback.context, *callback.tsd, id, callback.change);
        });
    }

    static void notify_item(OutputChangeContext &context, SubscribedTsd &tsd, item_id_t id, OutputChange change) {
        auto *entry = tsd.keys.find(id);
        if (entry == nullptr) {
            return;
        }

        const auto value_changed = refresh_reference_value(tsd, id, *entry, context.evaluation_time());
        if (tsd.stores_references()) {
            if (value_changed) {
                notify_selection(context, *entry,
                                 entry->selected_reference ? OutputChange::update : OutputChange::invalidation);
            }
            return;
        }

        if (entry->selected_reference) {
            notify_selection(context, *entry, change);
        } else if (value_changed) {
            notify_selection(context, *entry, OutputChange::invalidation);
        }
    }

    static void release_queued_entries(void *value) noexcept {
        static_cast<TsdKeySubscriptions *>(value)->release_queued_entries();
    }

    void release_queued_entries() noexcept {
        cycle_callback_registered_ = false;
        const auto release_count = release_queue_.size();
        for (auto index = std::size_t{}; index < release_count; ++index) {
            auto *entry = release_queue_.back();
            release_queue_.pop_back();
            entry->clear_release_queue_index();
            if (!has_active_subscribers(*entry)) {
                erase_key(*entry, entry->id);
            }
        }

        auto index = std::size_t{};
        while (index < subscribed_tsds_.size()) {
            if (subscribed_tsds_[index]->keys.empty()) {
                erase_tsd(*subscribed_tsds_[index]);
            } else {
                ++index;
            }
        }
    }

    ReferenceProvider *provider_;
    std::pmr::vector<SubscribedTsd *> subscribed_tsds_;
    bool has_active_subscribers(const TsdKeySelection &entry) const noexcept {
        if (entry.tsd->stores_references()) {
            return entry.selection.reference_item_subscribers != nullptr &&
                   !entry.selection.reference_item_subscribers->empty();
        }
        return provider_->item_reference_has_subscribers(*entry.selection.value_item_reference);
    }

    std::pmr::vector<TsdKeySelection *> release_queue_;
    std::size_t entry_count_{};
    allocator memory_;
    bool cycle_callback_registered_{};
};

namespace {

const Reference &selected_tsd_key_reference(const void *value) noexcept {
    return static_cast<const TsdKeySelection *>(value)->selected_reference;
}

void subscribe_tsd_key_selection(void *value, RuntimeGraph &graph, std::uint32_t input_offset,
                                 std::size_t notification_data, std::uint32_t node_index,
                                 InputNotificationFunction notification) {
    auto &entry = *static_cast<TsdKeySelection *>(value);
    entry.tsd->owner->add_binding_subscriber(entry, graph, input_offset, notification_data, node_index, notification);
}

void unsubscribe_tsd_key_selection(void *value, RuntimeGraph &graph, std::size_t input_offset) noexcept {
    auto &entry = *static_cast<TsdKeySelection *>(value);
    entry.tsd->owner->remove_binding_subscriber(entry, graph, input_offset);
}

} // namespace

namespace {

TsdKeySubscriptions &require_tsd_key_subscriptions(Reference reference) {
    auto &provider = ReferenceProvider::require(reference);
    if (auto *subscriptions = provider.component<TsdKeySubscriptions>()) {
        return *subscriptions;
    }

    auto &memory = provider.allocation();
    auto *subscriptions = memory.new_object<TsdKeySubscriptions>(provider, memory);
    provider.add_component(*subscriptions);
    return *subscriptions;
}

} // namespace

TsdItemReferenceInput tsd_item_reference_input(Reference tsd, Scalar<> key) {
    return require_tsd_key_subscriptions(tsd).input(tsd, key);
}

const TimeSeriesRuntimeType &tsd_item_reference_runtime_type(const TimeSeriesRuntimeType &tsd) {
    return time_series_reference_runtime_type(
        resolved_tsd_item_reference_type(static_cast<const TimeSeriesDictionaryRuntimeType &>(tsd)));
}

static_assert(sizeof(TsdKeySelection) == 5 * sizeof(void *));

} // namespace bg::details
