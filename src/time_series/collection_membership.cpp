#include <bg/time_series/collection_membership.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <vector>

namespace bg::details {
namespace {

class CollectionMembershipSubscriptions;

struct ObservedCollection;

struct Membership {
    ObservedCollection *collection{};
    item_id_t id{util::occupancy_index::npos};
    TimeSeriesState last_updated;
    DynamicNodeSubscriberStorage<> subscribers;
    std::uint32_t release_index{};
    bool value{};

    bool queued_for_release() const noexcept { return release_index != 0; }
};

static_assert(sizeof(Membership) == 6 * sizeof(void *));

struct CollectionIdentity {
    Reference reference;
    const TimeSeriesRuntimeType *runtime_type;
    const TimeSeriesSetRuntimeType *retained_runtime_type;
    const void *retained_value;
    bool valid_items;

    friend bool operator==(const CollectionIdentity &, const CollectionIdentity &) = default;
};

struct CollectionIdentityHash {
    std::size_t operator()(const CollectionIdentity &identity) const noexcept {
        auto result = std::hash<std::uintptr_t>{}(identity.reference.raw());
        result ^= std::hash<const void *>{}(identity.runtime_type) + 0x9e3779b9 + (result << 6) + (result >> 2);
        result ^= std::hash<const void *>{}(identity.retained_runtime_type) + 0x9e3779b9 + (result << 6) +
                  (result >> 2);
        result ^= std::hash<const void *>{}(identity.retained_value) + 0x9e3779b9 + (result << 6) + (result >> 2);
        result ^= std::hash<bool>{}(identity.valid_items) + 0x9e3779b9 + (result << 6) + (result >> 2);
        return result;
    }
};

struct ObservedCollection {
    ObservedCollection(CollectionMembershipSubscriptions &owner, CollectionIdentity identity) noexcept
        : owner{&owner}, identity{identity}, value_runtime_type{&cpp_time_series_runtime_type<TS<bool>>()} {}

    CollectionMembershipSubscriptions *owner;
    CollectionIdentity identity;
    const TimeSeriesRuntimeType *value_runtime_type;
    util::hash_dictionary<item_id_t, Membership> memberships;
    bool observing{};
};

struct CollectionView {
    const TimeSeriesSetRuntimeType &runtime_type;
    const void *storage;
};

CollectionView collection_view(const TimeSeriesRuntimeType &runtime_type, const void *storage) {
    if (type_cast<TimeSeriesSetType>(&runtime_type.type()) != nullptr) {
        return {static_cast<const TimeSeriesSetRuntimeType &>(runtime_type), storage};
    }
    const auto item = runtime_type.item(storage, key_set_item_id);
    return {static_cast<const TimeSeriesSetRuntimeType &>(item.runtime_type), item.value};
}

class CollectionMembershipSubscriptions final : public ReferenceProviderComponent {
  public:
    CollectionMembershipSubscriptions(ReferenceProvider &provider, allocator memory)
        : provider_{&provider}, release_queue_{&memory.memory_resource()}, memory_{memory} {}

    ~CollectionMembershipSubscriptions() override { assert(collection_registry_.empty()); }

    DynamicValueBinding<bool> binding(Reference reference, const ScalarData &key, bool valid_items) {
        const auto resolved = ResolvedTimeSeries::resolve(reference);
        if (resolved.runtime_type == nullptr || resolved.storage == nullptr) {
            return {};
        }

        const auto view = collection_view(*resolved.runtime_type, resolved.storage);
        const auto retained = view.runtime_type.retain_key(const_cast<void *>(view.storage), key, memory_);
        if (retained.runtime_type == nullptr) {
            return {};
        }
        auto release_retention = util::scope_exit{[&]() noexcept {
            retained.runtime_type->release_key(retained.value, retained.id, memory_);
        }};

        auto created_collection = false;
        auto &collection =
            require_collection(reference, *resolved.runtime_type, retained, valid_items, created_collection);
        auto release_empty_collection = util::scope_exit{[&]() noexcept {
            if (created_collection && collection.memberships.empty()) {
                erase_collection(collection);
            }
        }};
        if (auto *existing = collection.memberships.find(retained.id)) {
            return {existing, &binding_type()};
        }

        const auto value = resolved.valid()
                               ? contains(collection, *resolved.runtime_type, resolved.storage, view, key)
                               : false;
        if (release_queue_.capacity() <= membership_count_) {
            release_queue_.reserve(std::max<std::size_t>(release_queue_.capacity() * 2, membership_count_ + 1));
        }
        ensure_cycle_callback();
        auto [membership, inserted] = collection.memberships.try_emplace(retained.id, memory_.memory_resource());
        assert(inserted);
        membership->collection = &collection;
        membership->id = retained.id;
        membership->value = value;
        if (resolved.valid()) {
            membership->last_updated.publish(provider_->owner().evaluation_time());
        }
        ++membership_count_;
        queue_release(*membership);
        release_empty_collection.release();
        release_retention.release();
        return {membership, &binding_type()};
    }

    void detach() noexcept override {
        provider_->remove_cycle_callback(this);
        cycle_callback_registered_ = false;
        for (const auto entry : collection_registry_) {
            auto *collection = entry.value;
            if (!collection->observing) {
                continue;
            }
            provider_->remove_observer(collection->identity.reference, provider_->owner(), collection);
            collection->observing = false;
        }
    }

    void notify_item_bindings(Reference, item_id_t, OutputChangeContext &) override {}

    void destroy(allocator &memory) noexcept override {
        release_all();
        memory.delete_object(this);
    }

  private:
    class BindingType final : public DynamicValueBindingType {
      public:
        TimeSeriesInputData input_data(const void *data, EvaluationTime evaluation_time) const noexcept override {
            const auto &membership = *static_cast<const Membership *>(data);
            return {*membership.collection->value_runtime_type, membership.last_updated, &membership.value,
                     evaluation_time != MIN_DT && membership.last_updated.time() == evaluation_time };
        }

        void subscribe(void *data, RuntimeGraph &graph, std::size_t storage_offset,
                       std::uint32_t node_index) const override {
            auto &membership = *static_cast<Membership *>(data);
            membership.subscribers.add(
                DynamicNodeSubscriber{graph, static_cast<std::uint32_t>(storage_offset), node_index, true, false, false},
                membership.collection->owner->memory_);
            membership.collection->owner->remove_from_release_queue(membership);
        }

        void unsubscribe(void *data, RuntimeGraph &graph, std::size_t storage_offset) const noexcept override {
            auto &membership = *static_cast<Membership *>(data);
            membership.subscribers.remove(graph, storage_offset);
            if (membership.subscribers.empty()) {
                membership.collection->owner->queue_release(membership);
            }
        }
    };

    static const DynamicValueBindingType &binding_type() noexcept {
        static const auto result = BindingType{};
        return result;
    }

    ObservedCollection &require_collection(Reference reference, const TimeSeriesRuntimeType &runtime_type,
                                           RetainedSetKey retained, bool valid_items, bool &created) {
        const auto identity = CollectionIdentity{reference, &runtime_type, retained.runtime_type, retained.value,
                                                 valid_items};
        if (auto *collection = collection_registry_.find(identity)) {
            return **collection;
        }

        auto *collection = memory_.new_object<ObservedCollection>(*this, identity);
        auto release_collection = util::scope_exit{[&]() noexcept { release_collection_storage(*collection); }};
        const auto [registered, inserted] =
            collection_registry_.try_emplace(identity, memory_.memory_resource(), collection);
        assert(inserted && *registered == collection);
        auto remove_registry = util::scope_exit{[&]() noexcept {
            collection_registry_.erase(identity, memory_.memory_resource());
        }};
        provider_->add_observer(reference, provider_->owner(), collection, &notify_collection);
        collection->observing = true;
        remove_registry.release();
        release_collection.release();
        created = true;
        return *collection;
    }

    static void notify_collection(OutputChangeContext &context, void *value, OutputChange change) {
        auto &collection = *static_cast<ObservedCollection *>(value);
        collection.owner->notify(context, collection, change);
    }

    void notify(OutputChangeContext &context, ObservedCollection &collection, OutputChange change) {
        const auto resolved = ResolvedTimeSeries::resolve(collection.identity.reference);
        if (change == OutputChange::invalidation || !resolved.valid()) {
            for (const auto entry : collection.memberships) {
                auto &membership = entry.value;
                if (membership.last_updated.valid()) {
                    membership.last_updated.invalidate(context.evaluation_time());
                    membership.subscribers.notify(context, OutputChange::invalidation);
                }
            }
            return;
        }

        const auto view = collection_view(*resolved.runtime_type, resolved.storage);
        if (backing_replaced(collection, view)) {
            const auto output_change = change == OutputChange::revalidation ? OutputChange::revalidation
                                                                            : OutputChange::update;
            for (const auto item : collection.memberships) {
                auto &membership = item.value;
                const auto was_valid = membership.last_updated.valid();
                const auto &owner_type = *collection.identity.retained_runtime_type;
                const auto key = ScalarData{owner_type.key_runtime_type(),
                                            owner_type.set_key(collection.identity.retained_value, membership.id)};
                membership.value = contains(collection, *resolved.runtime_type, resolved.storage, view, key);
                membership.last_updated.publish(context.evaluation_time());
                membership.subscribers.notify(context, was_valid ? output_change : OutputChange::revalidation);
            }
            return;
        }

        if (change == OutputChange::revalidation) {
            for (const auto item : collection.memberships) {
                refresh(context, *resolved.runtime_type, resolved, view, item.value, true);
            }
            return;
        }

        const auto notify_id = [&](item_id_t id) {
            auto *membership = static_cast<Membership *>(nullptr);
            if (collection.identity.retained_runtime_type == &view.runtime_type &&
                collection.identity.retained_value == view.storage) {
                membership = collection.memberships.find(id);
                if (membership == nullptr) {
                    return;
                }
            } else {
                const auto *key = view.runtime_type.set_key(view.storage, id);
                const auto key_data = ScalarData{view.runtime_type.key_runtime_type(), key};
                const auto retained_id = collection.identity.retained_runtime_type->find_set_key(
                    collection.identity.retained_value, key_data);
                if (retained_id == util::occupancy_index::npos) {
                    return;
                }
                membership = collection.memberships.find(retained_id);
            }
            if (membership != nullptr) {
                refresh(context, *resolved.runtime_type, resolved, view, *membership, false, id);
            }
        };

        for_each_changed_id(collection.identity.valid_items, resolved, notify_id);
    }

    bool backing_replaced(ObservedCollection &collection, CollectionView view) {
        if (collection.memberships.empty() ||
            (collection.identity.retained_runtime_type == &view.runtime_type &&
             collection.identity.retained_value == view.storage)) {
            return false;
        }

        const auto first = collection.memberships.begin();
        const auto &membership = (*first).value;
        const auto &owner_type = *collection.identity.retained_runtime_type;
        const auto key = ScalarData{owner_type.key_runtime_type(), owner_type.set_key(
                                                                       collection.identity.retained_value,
                                                                       membership.id)};
        const auto retained = view.runtime_type.retain_key(const_cast<void *>(view.storage), key, memory_);
        if (retained.runtime_type == nullptr) {
            return false;
        }
        auto release_retention = util::scope_exit{[&]() noexcept {
            retained.runtime_type->release_key(retained.value, retained.id, memory_);
        }};
        return retained.runtime_type != collection.identity.retained_runtime_type ||
               retained.value != collection.identity.retained_value;
    }

    bool contains(const ObservedCollection &collection, const TimeSeriesRuntimeType &runtime_type,
                  const void *storage, CollectionView view, const ScalarData &key,
                  item_id_t known_id = util::occupancy_index::npos) const {
        if (!view.runtime_type.set_contains(view.storage, key)) {
            return false;
        }
        if (!collection.identity.valid_items ||
            type_cast<TimeSeriesDictionaryType>(&runtime_type.type()) == nullptr) {
            return true;
        }
        const auto &dictionary = static_cast<const TimeSeriesDictionaryRuntimeType &>(runtime_type);
        auto id = util::occupancy_index::npos;
        if (known_id == util::occupancy_index::npos) {
            id = dictionary.find_item(storage, key);
        } else if (dictionary.key_live(storage, known_id)) {
            id = known_id;
        }
        return id != util::occupancy_index::npos && dictionary.item(storage, id).last_updated.valid();
    }

    template <typename Function>
    static void for_each_changed_id(bool valid_items, const ResolvedTimeSeries &resolved, Function function) {
        if (type_cast<TimeSeriesDictionaryType>(&resolved.runtime_type->type()) == nullptr) {
            const auto &runtime_type = static_cast<const TimeSeriesSetRuntimeType &>(*resolved.runtime_type);
            for (auto range : {TimeSeriesSetRuntimeType::Range::added, TimeSeriesSetRuntimeType::Range::removed}) {
                auto id = runtime_type.set_next_item_id(resolved.last_updated, resolved.storage, range, 0);
                while (id != util::occupancy_index::npos) {
                    function(id);
                    id = runtime_type.set_next_item_id(resolved.last_updated, resolved.storage, range, id + 1);
                }
            }
            return;
        }

        const auto &runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*resolved.runtime_type);
        for (const auto range : {TimeSeriesDictionaryRuntimeType::ItemRange::added,
                                 TimeSeriesDictionaryRuntimeType::ItemRange::removed}) {
            auto id = runtime_type.next_item_id(resolved.last_updated, resolved.storage, range, 0);
            while (id != util::occupancy_index::npos) {
                function(id);
                id = runtime_type.next_item_id(resolved.last_updated, resolved.storage, range, id + 1);
            }
        }
        if (!valid_items) {
            return;
        }
        auto id = runtime_type.next_item_id(resolved.last_updated, resolved.storage,
                                            TimeSeriesDictionaryRuntimeType::ItemRange::updated, 0);
        while (id != util::occupancy_index::npos) {
            function(id);
            id = runtime_type.next_item_id(resolved.last_updated, resolved.storage,
                                          TimeSeriesDictionaryRuntimeType::ItemRange::updated, id + 1);
        }
    }

    void refresh(OutputChangeContext &context, const TimeSeriesRuntimeType &runtime_type,
                 const ResolvedTimeSeries &resolved, CollectionView view, Membership &membership, bool revalidation,
                 item_id_t known_id = util::occupancy_index::npos) {
        const auto &owner_type = *membership.collection->identity.retained_runtime_type;
        const auto key = ScalarData{owner_type.key_runtime_type(),
                                    owner_type.set_key(membership.collection->identity.retained_value, membership.id)};
        const auto value = contains(*membership.collection, runtime_type, resolved.storage, view, key, known_id);
        if (!membership.last_updated.valid()) {
            membership.value = value;
            membership.last_updated.publish(context.evaluation_time());
            membership.subscribers.notify(context, OutputChange::revalidation);
            return;
        }
        if (membership.value == value) {
            return;
        }
        membership.value = value;
        membership.last_updated.publish(context.evaluation_time());
        membership.subscribers.notify(context, revalidation ? OutputChange::revalidation : OutputChange::update);
    }

    void queue_release(Membership &membership) noexcept {
        if (membership.queued_for_release()) {
            return;
        }
        assert(release_queue_.size() < release_queue_.capacity());
        release_queue_.push_back(&membership);
        membership.release_index = static_cast<std::uint32_t>(release_queue_.size());
        if (!cycle_callback_registered_) {
            provider_->add_cycle_callback(this, &release_queued_memberships);
            cycle_callback_registered_ = true;
        }
    }

    void ensure_cycle_callback() {
        if (cycle_callback_registered_) {
            return;
        }
        provider_->add_cycle_callback(this, &release_queued_memberships);
        cycle_callback_registered_ = true;
    }

    void remove_from_release_queue(Membership &membership) noexcept {
        if (!membership.queued_for_release()) {
            return;
        }
        const auto index = membership.release_index - 1;
        auto *replacement = release_queue_.back();
        release_queue_[index] = replacement;
        replacement->release_index = static_cast<std::uint32_t>(index + 1);
        release_queue_.pop_back();
        membership.release_index = 0;
    }

    static void release_queued_memberships(void *value, EvaluationContext &) noexcept {
        auto &owner = *static_cast<CollectionMembershipSubscriptions *>(value);
        owner.cycle_callback_registered_ = false;
        const auto count = owner.release_queue_.size();
        for (auto index = std::size_t{}; index < count; ++index) {
            auto *membership = owner.release_queue_.back();
            owner.release_queue_.pop_back();
            membership->release_index = 0;
            if (!membership->subscribers.empty()) {
                continue;
            }
            auto *collection = membership->collection;
            owner.erase_membership(*membership);
            if (collection->memberships.empty()) {
                owner.erase_collection(*collection);
            }
        }
    }

    void erase_membership(Membership &membership) noexcept {
        auto &collection = *membership.collection;
        collection.identity.retained_runtime_type->release_key(
            const_cast<void *>(collection.identity.retained_value), membership.id, memory_);
        membership.subscribers.release();
        collection.memberships.erase_with(membership.id, memory_.memory_resource(), [](item_id_t *) noexcept {},
                                          [](Membership *) noexcept {});
        --membership_count_;
    }

    void erase_collection(ObservedCollection &collection) noexcept {
        collection_registry_.erase(collection.identity, memory_.memory_resource());
        release_collection_storage(collection);
    }

    void release_collection_storage(ObservedCollection &collection) noexcept {
        if (collection.observing) {
            provider_->remove_observer(collection.identity.reference, provider_->owner(), &collection);
        }
        for (const auto item : collection.memberships) {
            auto &membership = item.value;
            collection.identity.retained_runtime_type->release_key(
                const_cast<void *>(collection.identity.retained_value), membership.id, memory_);
            membership.subscribers.release();
        }
        collection.memberships.release_with(memory_.memory_resource(), [](item_id_t *) noexcept {},
                                            [](Membership *) noexcept {});
        memory_.delete_object(&collection);
    }

    void release_all() noexcept {
        release_queue_.clear();
        for (const auto entry : collection_registry_) {
            release_collection_storage(*entry.value);
        }
        collection_registry_.release(memory_.memory_resource());
        membership_count_ = 0;
    }

    ReferenceProvider *provider_;
    util::hash_dictionary<CollectionIdentity, ObservedCollection *, CollectionIdentityHash> collection_registry_;
    std::pmr::vector<Membership *> release_queue_;
    std::size_t membership_count_{};
    allocator memory_;
    bool cycle_callback_registered_{};
};

} // namespace

DynamicValueBinding<bool> tss_membership_binding(Reference collection, Scalar<> key) {
    if (!collection) {
        return {};
    }
    auto &provider = ReferenceProvider::require(collection);
    auto &subscriptions = provider.require_component<CollectionMembershipSubscriptions>();
    return subscriptions.binding(collection, scalar_data(key), false);
}

DynamicValueBinding<bool> tsd_membership_binding(Reference collection, Scalar<> key, bool valid_items) {
    if (!collection) {
        return {};
    }
    auto &provider = ReferenceProvider::require(collection);
    auto &subscriptions = provider.require_component<CollectionMembershipSubscriptions>();
    return subscriptions.binding(collection, scalar_data(key), valid_items);
}

} // namespace bg::details
