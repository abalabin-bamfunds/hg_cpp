#include <bg/time_series/tsd/tsd_key_subscription.hpp>

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {

class TsdKeySubscriptions;

namespace {

struct TsdKeyEntry {
    bool pending_release() const noexcept { return retired_entry_ != 0; }

    std::uint32_t retired_entry_index() const noexcept {
        assert(pending_release());
        return retired_entry_ - 1;
    }

    void set_retired_entry_index(std::uint32_t index) noexcept { retired_entry_ = index + 1; }

    union {
        ItemReference *item_reference{};
        const Reference *stored_reference;
    };
    DynamicSubscribers *selection_subscribers{};
    EvaluationTime reference_last_updated{MIN_DT};
    bool item_valid{};
    bool retained{};

  private:
    std::uint32_t retired_entry_{};
};

using TsdKeyEntryMap = util::hash_dictionary<item_id_t, TsdKeyEntry>;

struct SubscribedTsd {
    SubscribedTsd(TsdKeySubscriptions &owner, Reference reference, RetainedDictionaryKey retained) noexcept
        : owner{&owner}, reference{reference}, runtime_type{retained.runtime_type}, storage{retained.value} {}

    TsdKeySubscriptions *owner;
    Reference reference;
    const TimeSeriesDictionaryRuntimeType *runtime_type;
    void *storage;
    TsdKeyEntryMap keys;
    bool observing_reference{};

    bool stores_references() const noexcept {
        return dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&runtime_type->item_runtime_type()) != nullptr;
    }
};

} // namespace

namespace {

class TsdKeyReferenceRuntimeType final : public TimeSeriesReferenceRuntimeType {
  public:
    TsdKeyReferenceRuntimeType(const TimeSeriesReferenceType &type, TsdKeySubscriptions &owner,
                               bool stored_reference) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(TsdKeyEntry), alignof(TsdKeyEntry)}, owner_{&owner},
          stored_reference_{stored_reference} {}

    bool reads_stored_reference() const noexcept { return stored_reference_; }
    Reference reference(const void *value) const noexcept override;
    void *binding_address(const void *value) const noexcept override { return const_cast<void *>(value); }
    Reference resolve_binding(const void *address) const noexcept override { return reference(address); }
    void add_binding_subscriber(void *address, RuntimeGraph &graph, std::uint32_t input_offset,
                                std::size_t notification_data, std::uint32_t node_index,
                                InputNotificationFunction function) const override;
    void remove_binding_subscriber(void *address, RuntimeGraph &graph,
                                   std::size_t input_offset) const noexcept override;

  private:
    TsdKeySubscriptions *owner_;
    bool stored_reference_;
};

const TimeSeriesReferenceType &resolved_tsd_item_reference_type(const TimeSeriesDictionaryRuntimeType &tsd) {
    const auto &dictionary_type = static_cast<const TimeSeriesDictionaryType &>(tsd.type());
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&dictionary_type.value_type())) {
        return *reference;
    }

    return time_series_reference_type(dictionary_type.value_type(), *dictionary_type.guarantee().value());
}

} // namespace

class TsdKeySubscriptions final : public ReferenceProviderComponent {
    struct RetiredKey {
        SubscribedTsd *tsd;
        item_id_t id;
        EvaluationTime time;
    };

  public:
    TsdKeySubscriptions(ReferenceProvider &provider, allocator memory)
        : provider_{&provider}, subscribed_tsds_{&memory.memory_resource()},
          retired_keys_{&memory.memory_resource()}, binding_runtime_types_{&memory.memory_resource()},
          memory_{memory} {}

    ~TsdKeySubscriptions() override {
        detach();
        for (auto *tsd : subscribed_tsds_) {
            release_tsd(*tsd);
        }
        for (auto *runtime_type : binding_runtime_types_) {
            memory_.delete_object(runtime_type);
        }
    }

    void detach() noexcept override {
        for (auto *tsd : subscribed_tsds_) {
            if (tsd->observing_reference) {
                remove_reference_observer(tsd->reference, provider_->owner(), tsd);
                tsd->observing_reference = false;
            }
        }
    }

    void destroy(allocator &memory) noexcept override { memory.delete_object(this); }

    [[nodiscard]] TsdItemReferenceInput input(Reference reference, Scalar<> key) {
        release_retired(provider_->owner().evaluation_time());

        const auto retained = retain_key(reference, key);
        if (retained.runtime_type == nullptr) {
            return {&MIN_DT, nullptr, nullptr, false};
        }
        auto release_retention = util::scope_exit{
            [&]() noexcept { retained.runtime_type->release_key(retained.value, retained.id, memory_); }};

        auto &tsd = require_tsd(reference, retained);
        auto &entry = require_entry(tsd, retained.id);
        if (!entry.retained) {
            entry.retained = true;
            release_retention.release();
        }

        refresh_reference_value(tsd, retained.id, entry, provider_->owner().evaluation_time());
        retire_if_unused(tsd, retained.id, provider_->owner().evaluation_time());

        const auto &runtime_type = binding_runtime_type(tsd);
        return {&entry.reference_last_updated, &runtime_type, &entry,
                entry.reference_last_updated == provider_->owner().evaluation_time()};
    }

    void add_binding_subscriber(TsdKeyEntry &entry, RuntimeGraph &graph, std::uint32_t input_offset,
                                std::size_t notification_data, std::uint32_t node_index,
                                InputNotificationFunction function) {
        auto &subscribers = DynamicSubscribers::ensure(entry.selection_subscribers, memory_);
        subscribers.add(graph, input_offset, notification_data, node_index, false, false, function);
    }

    static void remove_binding_subscriber(TsdKeyEntry &entry, RuntimeGraph &graph,
                                          std::size_t input_offset) noexcept {
        if (entry.selection_subscribers != nullptr) {
            entry.selection_subscribers->remove(graph, input_offset);
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(const EvaluationTime &, const void *, void *,
                                                                     TimeSeriesRuntimeType::ItemIdFunction) const;

    RetainedDictionaryKey retain_key(Reference reference, Scalar<> key) {
        const auto resolved = resolve_reference(reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(resolved.runtime_type);
        if (runtime_type == nullptr || resolved.storage == nullptr) {
            return {};
        }

        return runtime_type->retain_key(const_cast<void *>(resolved.storage), scalar_data(key), memory_);
    }

    TsdKeyEntry &require_entry(SubscribedTsd &tsd, item_id_t id) {
        if (auto *entry = tsd.keys.find(id)) {
            return *entry;
        }
        if (retired_keys_.capacity() <= entry_count_) {
            retired_keys_.reserve(std::max<std::size_t>(retired_keys_.capacity() * 2, 8));
        }

        auto *item_reference = tsd.stores_references() ? nullptr : memory_.new_object<ItemReference>(tsd.reference, id);
        auto release_item_reference = util::scope_exit{[&]() noexcept { memory_.delete_object(item_reference); }};
        auto [entry, inserted] = tsd.keys.try_emplace(id, memory_);
        assert(inserted);

        if (tsd.stores_references()) {
            entry->stored_reference = static_cast<const Reference *>(tsd.runtime_type->item(tsd.storage, id).value);
        } else {
            entry->item_reference = item_reference;
            release_item_reference.release();
        }
        ++entry_count_;
        return *entry;
    }

    const TsdKeyReferenceRuntimeType &binding_runtime_type(SubscribedTsd &tsd) {
        const auto &type = resolved_tsd_item_reference_type(*tsd.runtime_type);
        const auto stored_reference = tsd.stores_references();
        const auto existing = std::ranges::find_if(binding_runtime_types_, [&](const auto *candidate) {
            return &candidate->type() == &type && candidate->reads_stored_reference() == stored_reference;
        });
        if (existing != binding_runtime_types_.end()) {
            return **existing;
        }

        auto *result = memory_.new_object<TsdKeyReferenceRuntimeType>(type, *this, stored_reference);
        auto release = util::scope_exit{[&]() noexcept { memory_.delete_object(result); }};
        binding_runtime_types_.push_back(result);
        release.release();
        return *result;
    }

    static void refresh_reference_value(SubscribedTsd &tsd, item_id_t id, TsdKeyEntry &entry,
                                        EvaluationTime evaluation_time) {
        const auto was_valid = entry.reference_last_updated > MIN_DT;
        const auto key_is_live = tsd.runtime_type->key_live(tsd.storage, id);
        const auto item = tsd.runtime_type->item(tsd.storage, id);
        const auto item_is_reference =
            dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&item.runtime_type) != nullptr;
        const auto is_valid = key_is_live && (!item_is_reference || item.last_updated > MIN_DT);
        entry.item_valid = key_is_live && item.last_updated > MIN_DT;

        if (!is_valid) {
            entry.reference_last_updated = MIN_DT;
        } else if (!was_valid) {
            entry.reference_last_updated = evaluation_time;
        }
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
        add_reference_observer(reference, provider_->owner(), result, &notify_tsd);
        result->observing_reference = true;

        remove_entry.release();
        release.release();
        return *result;
    }

    void retire_if_unused(SubscribedTsd &tsd, item_id_t id, EvaluationTime evaluation_time) noexcept {
        auto *entry = tsd.keys.find(id);
        assert(entry != nullptr);
        if (!entry->pending_release()) {
            retired_keys_.push_back({&tsd, id, evaluation_time});
            entry->set_retired_entry_index(static_cast<std::uint32_t>(retired_keys_.size() - 1));
        } else {
            retired_keys_[entry->retired_entry_index()].time = evaluation_time;
        }
    }

    void erase_key(SubscribedTsd &tsd, item_id_t id) noexcept {
        auto *entry = tsd.keys.find(id);
        if (entry->retained) {
            tsd.runtime_type->release_key(tsd.storage, id, memory_);
        }
        DynamicSubscribers::release(entry->selection_subscribers);
        if (!tsd.stores_references()) {
            memory_.delete_object(entry->item_reference);
        }
        tsd.keys.erase_with(
            id, memory_, [](item_id_t *) noexcept {},
            [](TsdKeyEntry *value) noexcept { std::destroy_at(value); });
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
            remove_reference_observer(tsd.reference, provider_->owner(), &tsd);
        }
        for (const auto entry : tsd.keys) {
            if (entry.value.retained) {
                tsd.runtime_type->release_key(tsd.storage, entry.key, memory_);
            }
            DynamicSubscribers::release(entry.value.selection_subscribers);
            if (!tsd.stores_references()) {
                memory_.delete_object(entry.value.item_reference);
            }
        }
        tsd.keys.release_with(
            memory_, [](item_id_t *) noexcept {},
            [](TsdKeyEntry *entry) noexcept { std::destroy_at(entry); });
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

        const auto resolved = resolve_reference(tsd.reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(resolved.runtime_type);
        if (runtime_type == nullptr || resolved.storage == nullptr) {
            invalidate_all(context, tsd);
            return;
        }
        if (runtime_type != tsd.runtime_type || resolved.storage != tsd.storage) {
            invalidate_all(context, tsd);
            return;
        }

        if (change != OutputChange::update) {
            notify_present(context, tsd, resolved, *runtime_type, change);
            return;
        }

        notify_changes(context, tsd, resolved, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_added,
                       OutputChange::revalidation);
        notify_changes(context, tsd, resolved, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_removed,
                       OutputChange::invalidation);
        notify_changes(context, tsd, resolved, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_modified,
                       OutputChange::update);
    }

    static void notify_selection(OutputChangeContext &context, TsdKeyEntry &entry, OutputChange change) {
        context.notify_subscribers(entry.selection_subscribers, change);
    }

    static void notify_item_reference(OutputChangeContext &context, TsdKeyEntry &entry, OutputChange change) {
        notify_reference(Reference{entry.item_reference}, context, change);
    }

    static void invalidate_all(OutputChangeContext &context, SubscribedTsd &tsd) {
        for (const auto entry : tsd.keys) {
            const auto reference_was_valid = entry.value.reference_last_updated > MIN_DT;
            const auto item_was_valid = entry.value.item_valid;
            entry.value.reference_last_updated = MIN_DT;
            entry.value.item_valid = false;

            if (reference_was_valid) {
                notify_selection(context, entry.value, OutputChange::invalidation);
            }
            if (!tsd.stores_references() && item_was_valid) {
                notify_item_reference(context, entry.value, OutputChange::invalidation);
            }
        }
    }

    void release_retained_keys(SubscribedTsd &tsd) noexcept {
        for (const auto entry : tsd.keys) {
            if (!entry.value.retained) {
                continue;
            }

            tsd.runtime_type->release_key(tsd.storage, entry.key, memory_);
            entry.value.retained = false;
        }
    }

    static void notify_present(OutputChangeContext &context, SubscribedTsd &tsd, ResolvedTimeSeries resolved,
                               const TimeSeriesDictionaryRuntimeType &runtime_type, OutputChange change) {
        for (const auto entry : tsd.keys) {
            if (!runtime_type.key_live(resolved.storage, entry.key)) {
                continue;
            }

            notify_item(context, tsd, entry.key, change);
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

        const auto reference_was_valid = entry->reference_last_updated > MIN_DT;
        const auto item_was_valid = entry->item_valid;
        refresh_reference_value(tsd, id, *entry, context.evaluation_time());
        const auto reference_is_valid = entry->reference_last_updated > MIN_DT;

        if (tsd.stores_references()) {
            if (reference_was_valid != reference_is_valid) {
                notify_selection(context, *entry,
                                 reference_is_valid ? OutputChange::revalidation : OutputChange::invalidation);
            } else if (reference_is_valid && change == OutputChange::update) {
                entry->reference_last_updated = context.evaluation_time();
                notify_selection(context, *entry, OutputChange::update);
            }
            return;
        }

        if (reference_was_valid != reference_is_valid) {
            notify_selection(context, *entry,
                             reference_is_valid ? OutputChange::revalidation : OutputChange::invalidation);
        }
        if (item_was_valid != entry->item_valid) {
            notify_item_reference(context, *entry,
                                  entry->item_valid ? OutputChange::revalidation : OutputChange::invalidation);
        } else if (entry->item_valid && change == OutputChange::update) {
            notify_item_reference(context, *entry, OutputChange::update);
        }
    }

    void release_retired(EvaluationTime evaluation_time) noexcept {
        auto retained = retired_keys_.begin();
        for (const auto retired : retired_keys_) {
            auto *entry = retired.tsd->keys.find(retired.id);
            if (entry == nullptr) {
                continue;
            }
            if (retired.time >= evaluation_time) {
                *retained++ = retired;
                entry->set_retired_entry_index(static_cast<std::uint32_t>(retained - retired_keys_.begin() - 1));
                continue;
            }

            const auto has_selection_subscribers =
                entry->selection_subscribers != nullptr && !entry->selection_subscribers->empty();
            if (has_selection_subscribers ||
                (!retired.tsd->stores_references() && !provider_->release_item_reference(*entry->item_reference))) {
                *retained++ = retired;
                entry->set_retired_entry_index(static_cast<std::uint32_t>(retained - retired_keys_.begin() - 1));
                continue;
            }
            erase_key(*retired.tsd, retired.id);
        }
        retired_keys_.erase(retained, retired_keys_.end());

        auto tsd = std::size_t{};
        while (tsd < subscribed_tsds_.size()) {
            if (subscribed_tsds_[tsd]->keys.empty()) {
                erase_tsd(*subscribed_tsds_[tsd]);
            } else {
                ++tsd;
            }
        }
    }

    ReferenceProvider *provider_;
    std::pmr::vector<SubscribedTsd *> subscribed_tsds_;
    std::pmr::vector<RetiredKey> retired_keys_;
    std::pmr::vector<TsdKeyReferenceRuntimeType *> binding_runtime_types_;
    std::size_t entry_count_{};
    allocator memory_;
};

namespace {

Reference TsdKeyReferenceRuntimeType::reference(const void *value) const noexcept {
    const auto &entry = *static_cast<const TsdKeyEntry *>(value);
    if (entry.reference_last_updated <= MIN_DT) {
        return {};
    }

    return stored_reference_ ? *entry.stored_reference : Reference{entry.item_reference};
}

void TsdKeyReferenceRuntimeType::add_binding_subscriber(void *address, RuntimeGraph &graph,
                                                        std::uint32_t input_offset,
                                                        std::size_t notification_data,
                                                        std::uint32_t node_index,
                                                        InputNotificationFunction function) const {
    owner_->add_binding_subscriber(*static_cast<TsdKeyEntry *>(address), graph, input_offset, notification_data,
                                   node_index, function);
}

void TsdKeyReferenceRuntimeType::remove_binding_subscriber(void *address, RuntimeGraph &graph,
                                                           std::size_t input_offset) const noexcept {
    owner_->remove_binding_subscriber(*static_cast<TsdKeyEntry *>(address), graph, input_offset);
}

TsdKeySubscriptions &require_tsd_key_subscriptions(Reference reference) {
    auto &provider = require_reference_provider(reference);
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

static_assert(sizeof(TsdKeyEntry) == 4 * sizeof(void *));

} // namespace bg::details
