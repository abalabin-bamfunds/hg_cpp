#include <bg/time_series/tsd/tsd_key_subscription.hpp>

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
    DynamicSubscribers *subscribers{};
    ReferenceRoute route;
    bool item_valid{};
    bool retained{};
    std::uint32_t retired{};
};

using TsdKeyEntryMap = util::hash_dictionary<TimeSeriesItemId, TsdKeyEntry>;

struct SubscribedTsd {
    SubscribedTsd(TsdKeySubscriptions &owner, ReferenceAddress reference, RetainedDictionaryKey retained) noexcept
        : owner{&owner}, reference{reference}, runtime_type{retained.runtime_type}, value{retained.value} {}

    TsdKeySubscriptions *owner;
    ReferenceAddress reference;
    const TimeSeriesDictionaryRuntimeType *runtime_type;
    void *value;
    TsdKeyEntryMap keys;
    bool registered{};
};

} // namespace

class TsdKeySubscriptions final : public ReferenceProviderComponent {
    struct RetiredKey {
        SubscribedTsd *tsd;
        TimeSeriesItemId id;
        EvaluationTime time;
    };

  public:
    TsdKeySubscriptions(ReferenceProvider &provider, std::pmr::memory_resource &memory)
        : provider_{&provider}, subscribed_tsds_{&memory}, retired_keys_{&memory}, memory_{&memory} {}

    ~TsdKeySubscriptions() override {
        detach();
        for (auto *tsd : subscribed_tsds_) {
            release_tsd(*tsd);
        }
    }

    void detach() noexcept override {
        for (auto *tsd : subscribed_tsds_) {
            if (tsd->registered) {
                remove_reference_observer(tsd->reference, provider_->owner(), tsd);
                tsd->registered = false;
            }
        }
    }

    void destroy(std::pmr::memory_resource &memory) noexcept override {
        std::destroy_at(this);
        memory.deallocate(this, sizeof(TsdKeySubscriptions), alignof(TsdKeySubscriptions));
    }

    [[nodiscard]] TsdKeySubscription add(ReferenceAddress reference, const ScalarData &key, RuntimeGraph &graph,
                                         std::uint32_t node_index) {
        release_retired(graph.evaluation_time());

        const auto target = reference_target(reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
        if (runtime_type == nullptr || target.value == nullptr) {
            return {};
        }

        auto retained = runtime_type->retain_key(const_cast<void *>(target.value), key, *memory_);
        if (retained.runtime_type == nullptr) {
            return {};
        }
        auto release_retention = util::scope_exit{[&]() noexcept {
            retained.runtime_type->release_key(retained.value, retained.id, *memory_);
        }};

        auto &tsd = require_tsd(reference, retained);
        if (retired_keys_.capacity() <= entry_count_) {
            retired_keys_.reserve(std::max<std::size_t>(retired_keys_.capacity() * 2, 8));
        }
        auto remove_empty_tsd = util::scope_exit{[&]() noexcept {
            if (tsd.keys.empty()) {
                erase_tsd(tsd);
            }
        }};
        auto [entry, inserted] = tsd.keys.try_emplace(retained.id, *memory_);
        auto remove_new_key = util::scope_exit{[&]() noexcept {
            if (inserted) {
                erase_key(tsd, retained.id);
            }
        }};

        if (inserted) {
            entry->route.bind_item(reference, retained.id);
            if (tsd.runtime_type->key_live(tsd.value, retained.id)) {
                entry->item_valid = tsd.runtime_type->item(tsd.value, retained.id).last_updated > MIN_DT;
            }
            ++entry_count_;
            entry->retained = true;
            release_retention.release();
        } else if (!entry->retained) {
            entry->retained = true;
            entry->item_valid = tsd.runtime_type->key_live(tsd.value, retained.id) &&
                                tsd.runtime_type->item(tsd.value, retained.id).last_updated > MIN_DT;
            release_retention.release();
        }

        auto &subscribers = DynamicSubscribers::ensure(entry->subscribers, *memory_);
        subscribers.add(graph, node_index, 0, node_index, true, false);

        remove_new_key.release();
        remove_empty_tsd.release();
        return TsdKeySubscription{&tsd, retained.id};
    }

    void remove(TsdKeySubscription subscription, RuntimeGraph &graph, std::uint32_t node_index) noexcept {
        auto &tsd = *static_cast<SubscribedTsd *>(subscription.tsd_);
        auto *entry = tsd.keys.find(subscription.item_);
        if (entry == nullptr || entry->subscribers == nullptr) {
            return;
        }
        entry->subscribers->remove(graph, node_index);
        if (!entry->subscribers->empty()) {
            return;
        }
        if (entry->retired == 0) {
            retired_keys_.push_back({&tsd, subscription.item_, graph.evaluation_time()});
            entry->retired = static_cast<std::uint32_t>(retired_keys_.size());
        } else {
            retired_keys_[entry->retired - 1].time = graph.evaluation_time();
        }
        if (entry->retained) {
            tsd.runtime_type->release_key(tsd.value, subscription.item_, *memory_);
            entry->retained = false;
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(const EvaluationTime &, const void *, void *,
                                                                     TimeSeriesRuntimeType::ItemIdFunction) const;

    SubscribedTsd *find_tsd(ReferenceAddress reference, RetainedDictionaryKey retained) const noexcept {
        const auto found = std::ranges::find_if(subscribed_tsds_, [&](const auto *candidate) {
            return candidate->reference == reference && candidate->runtime_type == retained.runtime_type &&
                   candidate->value == retained.value;
        });
        return found == subscribed_tsds_.end() ? nullptr : *found;
    }

    SubscribedTsd &require_tsd(ReferenceAddress reference, RetainedDictionaryKey retained) {
        if (auto *existing = find_tsd(reference, retained)) {
            return *existing;
        }

        auto *storage = memory_->allocate(sizeof(SubscribedTsd), alignof(SubscribedTsd));
        auto *result = std::construct_at(static_cast<SubscribedTsd *>(storage), *this, reference, retained);
        auto release = util::scope_exit{[&]() noexcept { release_tsd(*result); }};
        subscribed_tsds_.push_back(result);
        auto remove_entry = util::scope_exit{[&]() noexcept { subscribed_tsds_.pop_back(); }};
        add_reference_observer(reference, provider_->owner(), result, &notify_tsd);
        result->registered = true;
        remove_entry.release();
        release.release();
        return *result;
    }

    void erase_key(SubscribedTsd &tsd, TimeSeriesItemId id) noexcept {
        auto *entry = tsd.keys.find(id);
        if (entry->retained) {
            tsd.runtime_type->release_key(tsd.value, id, *memory_);
        }
        tsd.keys.erase_with(id, *memory_, [](TimeSeriesItemId *) noexcept {}, [](TsdKeyEntry *value) noexcept {
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
        if (tsd.registered) {
            remove_reference_observer(tsd.reference, provider_->owner(), &tsd);
        }
        for (const auto entry : tsd.keys) {
            if (entry.value.retained) {
                tsd.runtime_type->release_key(tsd.value, entry.key, *memory_);
            }
        }
        tsd.keys.release_with(*memory_, [](TimeSeriesItemId *) noexcept {}, [](TsdKeyEntry *entry) noexcept {
            DynamicSubscribers::release(entry->subscribers);
            std::destroy_at(entry);
        });
        std::destroy_at(&tsd);
        memory_->deallocate(&tsd, sizeof(SubscribedTsd), alignof(SubscribedTsd));
    }

    static void notify_tsd(OutputChangeContext &context, void *value, OutputChange change) {
        auto &tsd = *static_cast<SubscribedTsd *>(value);
        tsd.owner->notify(context, tsd, change);
    }

    void notify(OutputChangeContext &context, SubscribedTsd &tsd, OutputChange change) {
        const auto target = reference_target(tsd.reference);
        const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
        if (runtime_type == nullptr || target.value == nullptr) {
            if (change == OutputChange::invalidation) {
                notify_all(context, tsd, change);
            }
            return;
        }
        if (runtime_type != tsd.runtime_type || target.value != tsd.value) {
            notify_all(context, tsd, change);
            return;
        }

        if (change != OutputChange::update) {
            notify_present(context, tsd, target, *runtime_type, change);
            return;
        }

        notify_changes(context, tsd, target, *runtime_type,
                       &TimeSeriesDictionaryRuntimeType::for_each_added, OutputChange::revalidation);
        notify_changes(context, tsd, target, *runtime_type,
                       &TimeSeriesDictionaryRuntimeType::for_each_removed, OutputChange::invalidation);
        notify_changes(context, tsd, target, *runtime_type,
                       &TimeSeriesDictionaryRuntimeType::for_each_modified, OutputChange::update);
    }

    static void notify_all(OutputChangeContext &context, SubscribedTsd &tsd, OutputChange change) {
        for (const auto entry : tsd.keys) {
            context.notify(entry.value.subscribers, change);
        }
    }

    static void notify_present(OutputChangeContext &context, SubscribedTsd &tsd, ReferenceTarget target,
                               const TimeSeriesDictionaryRuntimeType &runtime_type, OutputChange change) {
        for (const auto entry : tsd.keys) {
            if (runtime_type.key_live(target.value, entry.key)) {
                entry.value.item_valid = runtime_type.item(target.value, entry.key).last_updated > MIN_DT;
                context.notify(entry.value.subscribers, change);
            }
        }
    }

    void notify_changes(OutputChangeContext &context, SubscribedTsd &tsd, ReferenceTarget target,
                        const TimeSeriesDictionaryRuntimeType &runtime_type, ChangeFunction changes,
                        OutputChange change) {
        struct Callback {
            TsdKeySubscriptions *owner;
            SubscribedTsd *tsd;
            const TimeSeriesDictionaryRuntimeType *runtime_type;
            ReferenceTarget target;
            OutputChangeContext *context;
            OutputChange change;
        } callback{this, &tsd, &runtime_type, target, &context, change};

        (runtime_type.*changes)(*target.last_updated, target.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<Callback *>(value);
            callback.owner->notify_item(*callback.context, *callback.tsd, *callback.runtime_type,
                                        callback.target, id, callback.change);
        });
    }

    static void notify_item(OutputChangeContext &context, SubscribedTsd &tsd,
                            const TimeSeriesDictionaryRuntimeType &runtime_type, ReferenceTarget target,
                            TimeSeriesItemId id, OutputChange change) {
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

        const auto item = runtime_type.item(target.value, id);
        const auto item_valid = item.last_updated > MIN_DT;
        if (item_valid != entry->item_valid) {
            entry->item_valid = item_valid;
            context.notify(entry->subscribers,
                           item_valid ? OutputChange::revalidation : OutputChange::invalidation);
            return;
        }
        if (item_valid) {
            notify_reference(ReferenceAddress{&entry->route}, context, OutputChange::update);
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
                entry->retired = 0;
                continue;
            }
            if (retired.time >= evaluation_time || !provider_->release_route(entry->route)) {
                *retained++ = retired;
                entry->retired = static_cast<std::uint32_t>(retained - retired_keys_.begin());
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
    std::pmr::memory_resource *memory_;
};

namespace {

TsdKeySubscriptions &require_tsd_key_subscriptions(ReferenceAddress address) {
    auto &provider = require_reference_provider(address);
    if (auto *subscriptions = provider.component<TsdKeySubscriptions>()) {
        return *subscriptions;
    }

    auto &memory = provider.memory();
    auto *storage = memory.allocate(sizeof(TsdKeySubscriptions), alignof(TsdKeySubscriptions));
    auto release = util::scope_exit{
        [&]() noexcept { memory.deallocate(storage, sizeof(TsdKeySubscriptions), alignof(TsdKeySubscriptions)); }};
    auto *subscriptions = std::construct_at(static_cast<TsdKeySubscriptions *>(storage), provider, memory);
    provider.add_component(*subscriptions);
    release.release();
    return *subscriptions;
}

} // namespace

TsdKeySubscription add_tsd_key_subscriber(ReferenceAddress address, const ScalarData &key, RuntimeGraph &graph,
                                          std::uint32_t node_index) {
    return require_tsd_key_subscriptions(address).add(address, key, graph, node_index);
}

bool tsd_key_subscription_matches(TsdKeySubscription subscription, ReferenceAddress reference,
                                  const ScalarData &key) {
    if (!subscription) {
        return false;
    }

    const auto &tsd = *static_cast<const SubscribedTsd *>(subscription.tsd_);
    if (tsd.reference != reference) {
        return false;
    }

    const auto target = reference_target(reference);
    const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
    if (runtime_type == nullptr || target.value == nullptr) {
        return false;
    }
    const auto storage = runtime_type->retention_storage(const_cast<void *>(target.value));
    return storage.runtime_type == tsd.runtime_type && storage.value == tsd.value &&
           tsd.runtime_type->key_runtime_type().equals(tsd.runtime_type->key(tsd.value, subscription.item_), key);
}

ReferenceAddress tsd_key_subscription_target(TsdKeySubscription subscription) {
    if (!subscription) {
        return {};
    }

    auto &tsd = *static_cast<SubscribedTsd *>(subscription.tsd_);
    if (!tsd.runtime_type->key_live(tsd.value, subscription.item_)) {
        return {};
    }

    auto *entry = tsd.keys.find(subscription.item_);
    assert(entry != nullptr);
    const auto item = tsd.runtime_type->item(tsd.value, subscription.item_);
    if (item.last_updated <= MIN_DT) {
        return {};
    }
    if (dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&item.runtime_type) != nullptr) {
        return item.runtime_type.reference_address(item.value);
    }

    return ReferenceAddress{&entry->route};
}

void remove_tsd_key_subscriber(TsdKeySubscription subscription, RuntimeGraph &graph,
                               std::uint32_t node_index) noexcept {
    if (subscription) {
        static_cast<const SubscribedTsd *>(subscription.tsd_)->owner->remove(subscription, graph, node_index);
    }
}

} // namespace bg::details
