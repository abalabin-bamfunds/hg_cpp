#include <bg/time_series/tsd/tsd_key_subscription.hpp>

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {
namespace {

struct TsdKeyEntry {
    bool pending_release() const noexcept { return retired_entry_ != 0; }
    std::uint32_t retired_entry_index() const noexcept {
        assert(pending_release());
        return retired_entry_ - 1;
    }
    void set_retired_entry_index(std::uint32_t index) noexcept { retired_entry_ = index + 1; }
    void clear_retired_entry() noexcept { retired_entry_ = 0; }

    DynamicSubscribers *subscribers{};
    ItemReference reference;
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
};

} // namespace

class TsdKeySubscriptions final : public ReferenceProviderComponent {
    struct RetiredKey {
        SubscribedTsd *tsd;
        item_id_t id;
        EvaluationTime time;
    };

  public:
    TsdKeySubscriptions(ReferenceProvider &provider, allocator memory)
        : provider_{&provider}, subscribed_tsds_{&memory.memory_resource()}, retired_keys_{&memory.memory_resource()},
          memory_{memory} {}

    ~TsdKeySubscriptions() override {
        detach();
        for (auto *tsd : subscribed_tsds_) {
            release_tsd(*tsd);
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

    void destroy(allocator &memory) noexcept override {
        memory.delete_object(this);
    }

    [[nodiscard]] TsdKeySubscription add(Reference reference, const ScalarData &key, RuntimeGraph &graph,
                                         std::uint32_t node_index) {
        release_retired(graph.evaluation_time());

        const auto resolved = resolve_reference(reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(resolved.runtime_type);
        if (runtime_type == nullptr || resolved.storage == nullptr) {
            return {};
        }

        auto retained = runtime_type->retain_key(const_cast<void *>(resolved.storage), key, memory_);
        if (retained.runtime_type == nullptr) {
            return {};
        }
        auto release_retention = util::scope_exit{
            [&]() noexcept { retained.runtime_type->release_key(retained.value, retained.id, memory_); }};

        auto &tsd = require_tsd(reference, retained);
        if (retired_keys_.capacity() <= entry_count_) {
            retired_keys_.reserve(std::max<std::size_t>(retired_keys_.capacity() * 2, 8));
        }
        auto remove_empty_tsd = util::scope_exit{[&]() noexcept {
            if (tsd.keys.empty()) {
                erase_tsd(tsd);
            }
        }};
        auto [entry, inserted] = tsd.keys.try_emplace(retained.id, memory_);
        auto remove_new_key = util::scope_exit{[&]() noexcept {
            if (inserted) {
                erase_key(tsd, retained.id);
            }
        }};

        if (inserted) {
            entry->reference = ItemReference{reference, retained.id};
            if (tsd.runtime_type->key_live(tsd.storage, retained.id)) {
                entry->item_valid = tsd.runtime_type->item(tsd.storage, retained.id).last_updated > MIN_DT;
            }
            ++entry_count_;
            entry->retained = true;
            release_retention.release();
        } else if (!entry->retained) {
            entry->retained = true;
            entry->item_valid = tsd.runtime_type->key_live(tsd.storage, retained.id) &&
                                tsd.runtime_type->item(tsd.storage, retained.id).last_updated > MIN_DT;
            release_retention.release();
        }

        auto &subscribers = DynamicSubscribers::ensure(entry->subscribers, memory_);
        subscribers.add(graph, node_index, 0, node_index, true, false);

        remove_new_key.release();
        remove_empty_tsd.release();
        return TsdKeySubscription{&tsd, retained.id};
    }

    void remove(TsdKeySubscription subscription, RuntimeGraph &graph, std::uint32_t node_index) noexcept {
        auto &tsd = *static_cast<SubscribedTsd *>(subscription.subscribed_tsd_);
        auto *entry = tsd.keys.find(subscription.item_id_);
        if (entry == nullptr || entry->subscribers == nullptr) {
            return;
        }
        entry->subscribers->remove(graph, node_index);
        if (!entry->subscribers->empty()) {
            return;
        }
        if (!entry->pending_release()) {
            retired_keys_.push_back({&tsd, subscription.item_id_, graph.evaluation_time()});
            entry->set_retired_entry_index(static_cast<std::uint32_t>(retired_keys_.size() - 1));
        } else {
            retired_keys_[entry->retired_entry_index()].time = graph.evaluation_time();
        }
        if (entry->retained) {
            tsd.runtime_type->release_key(tsd.storage, subscription.item_id_, memory_);
            entry->retained = false;
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(const EvaluationTime &, const void *, void *,
                                                                     TimeSeriesRuntimeType::ItemIdFunction) const;

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

    void erase_key(SubscribedTsd &tsd, item_id_t id) noexcept {
        auto *entry = tsd.keys.find(id);
        if (entry->retained) {
            tsd.runtime_type->release_key(tsd.storage, id, memory_);
        }
        tsd.keys.erase_with(
            id, memory_, [](item_id_t *) noexcept {},
            [](TsdKeyEntry *value) noexcept {
                DynamicSubscribers::release(value->subscribers);
                std::destroy_at(value);
            });
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
        }
        tsd.keys.release_with(
            memory_, [](item_id_t *) noexcept {},
            [](TsdKeyEntry *entry) noexcept {
                DynamicSubscribers::release(entry->subscribers);
                std::destroy_at(entry);
            });
        memory_.delete_object(&tsd);
    }

    static void notify_tsd(OutputChangeContext &context, void *value, OutputChange change) {
        auto &tsd = *static_cast<SubscribedTsd *>(value);
        tsd.owner->notify(context, tsd, change);
    }

    void notify(OutputChangeContext &context, SubscribedTsd &tsd, OutputChange change) {
        const auto resolved = resolve_reference(tsd.reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(resolved.runtime_type);
        if (runtime_type == nullptr || resolved.storage == nullptr) {
            if (change == OutputChange::invalidation) {
                notify_all(context, tsd, change);
            }
            return;
        }
        if (runtime_type != tsd.runtime_type || resolved.storage != tsd.storage) {
            notify_all(context, tsd, change);
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

    static void notify_all(OutputChangeContext &context, SubscribedTsd &tsd, OutputChange change) {
        for (const auto entry : tsd.keys) {
            context.notify(entry.value.subscribers, change);
        }
    }

    static void notify_present(OutputChangeContext &context, SubscribedTsd &tsd, ResolvedTimeSeries resolved,
                               const TimeSeriesDictionaryRuntimeType &runtime_type, OutputChange change) {
        for (const auto entry : tsd.keys) {
            if (runtime_type.key_live(resolved.storage, entry.key)) {
                entry.value.item_valid = runtime_type.item(resolved.storage, entry.key).last_updated > MIN_DT;
                context.notify(entry.value.subscribers, change);
            }
        }
    }

    void notify_changes(OutputChangeContext &context, SubscribedTsd &tsd, ResolvedTimeSeries resolved,
                        const TimeSeriesDictionaryRuntimeType &runtime_type, ChangeFunction changes,
                        OutputChange change) {
        struct Callback {
            TsdKeySubscriptions *owner;
            SubscribedTsd *tsd;
            const TimeSeriesDictionaryRuntimeType *runtime_type;
            ResolvedTimeSeries resolved;
            OutputChangeContext *context;
            OutputChange change;
        } callback{this, &tsd, &runtime_type, resolved, &context, change};

        (runtime_type.*changes)(*resolved.last_updated, resolved.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            callback.owner->notify_item(*callback.context, *callback.tsd, *callback.runtime_type, callback.resolved, id,
                                        callback.change);
        });
    }

    static void notify_item(OutputChangeContext &context, SubscribedTsd &tsd,
                            const TimeSeriesDictionaryRuntimeType &runtime_type, ResolvedTimeSeries resolved,
                            item_id_t id, OutputChange change) {
        auto *entry = tsd.keys.find(id);
        if (entry == nullptr) {
            return;
        }

        const auto item_is_reference =
            dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&runtime_type.item_runtime_type()) != nullptr;
        if (change != OutputChange::update || item_is_reference) {
            entry->item_valid = change != OutputChange::invalidation;
            context.notify(entry->subscribers, change);
            return;
        }

        const auto item = runtime_type.item(resolved.storage, id);
        const auto item_valid = item.last_updated > MIN_DT;
        if (item_valid != entry->item_valid) {
            entry->item_valid = item_valid;
            context.notify(entry->subscribers, item_valid ? OutputChange::revalidation : OutputChange::invalidation);
            return;
        }
        if (item_valid) {
            notify_reference(Reference{&entry->reference}, context, OutputChange::update);
        }
    }

    void release_retired(EvaluationTime evaluation_time) noexcept {
        auto retained = retired_keys_.begin();
        for (const auto retired : retired_keys_) {
            auto *entry = retired.tsd->keys.find(retired.id);
            if (entry == nullptr) {
                continue;
            }
            if (entry->subscribers != nullptr && !entry->subscribers->empty()) {
                entry->clear_retired_entry();
                continue;
            }
            if (retired.time >= evaluation_time || !provider_->release_item_reference(entry->reference)) {
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
    std::size_t entry_count_{};
    allocator memory_;
};

namespace {

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

TsdKeySubscription add_tsd_key_subscriber(Reference reference, const ScalarData &key, RuntimeGraph &graph,
                                          std::uint32_t node_index) {
    return require_tsd_key_subscriptions(reference).add(reference, key, graph, node_index);
}

bool tsd_key_subscription_matches(TsdKeySubscription subscription, Reference reference, const ScalarData &key) {
    if (!subscription) {
        return false;
    }

    const auto &tsd = *static_cast<const SubscribedTsd *>(subscription.subscribed_tsd_);
    if (tsd.reference != reference) {
        return false;
    }

    const auto resolved = resolve_reference(reference);
    const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(resolved.runtime_type);
    if (runtime_type == nullptr || resolved.storage == nullptr) {
        return false;
    }
    const auto storage = runtime_type->retention_storage(const_cast<void *>(resolved.storage));
    return storage.runtime_type == tsd.runtime_type && storage.value == tsd.storage &&
           tsd.runtime_type->key_runtime_type().equals(tsd.runtime_type->key(tsd.storage, subscription.item_id_), key);
}

Reference tsd_key_subscription_reference(TsdKeySubscription subscription) {
    if (!subscription) {
        return {};
    }

    auto &tsd = *static_cast<SubscribedTsd *>(subscription.subscribed_tsd_);
    if (!tsd.runtime_type->key_live(tsd.storage, subscription.item_id_)) {
        return {};
    }

    auto *entry = tsd.keys.find(subscription.item_id_);
    assert(entry != nullptr);
    const auto item = tsd.runtime_type->item(tsd.storage, subscription.item_id_);
    if (item.last_updated <= MIN_DT) {
        return {};
    }
    if (dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&item.runtime_type) != nullptr) {
        return item.runtime_type.reference(item.value);
    }

    return Reference{&entry->reference};
}

void remove_tsd_key_subscriber(TsdKeySubscription subscription, RuntimeGraph &graph,
                               std::uint32_t node_index) noexcept {
    if (subscription) {
        static_cast<const SubscribedTsd *>(subscription.subscribed_tsd_)
            ->owner->remove(subscription, graph, node_index);
    }
}

} // namespace bg::details
