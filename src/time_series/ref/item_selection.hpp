#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {

template <typename Collection> struct ItemSelection {
    bool queued_for_release() const noexcept { return (state_ & release_queue_index_mask) != 0; }

    std::uint32_t release_queue_index() const noexcept {
        assert(queued_for_release());
        return (state_ & release_queue_index_mask) - 1;
    }

    void set_release_queue_index(std::uint32_t index) noexcept {
        assert(index < release_queue_index_mask);
        state_ = (state_ & selection_flags) | (index + 1);
    }

    void clear_release_queue_index() noexcept { state_ &= selection_flags; }

    bool retained() const noexcept { return (state_ & retained_flag) != 0; }

    void set_retained(bool value) noexcept {
        state_ = (state_ & ~retained_flag) | (value ? retained_flag : 0);
    }

    bool stores_reference() const noexcept { return (state_ & stores_reference_flag) != 0; }

    void set_stores_reference(bool value) noexcept {
        state_ = (state_ & ~stores_reference_flag) | (value ? stores_reference_flag : 0);
    }

    Collection *collection{};
    union SelectionStorage {
        ItemReference *value_item_reference;
        DynamicSubscribers *reference_item_subscribers;

        SelectionStorage() : value_item_reference{} {}
    } selection;
    Reference selected_reference{};
    TimeSeriesState selection_time{};
    item_id_t id{};

  private:
    static constexpr auto retained_flag = std::uint32_t{1} << 31;
    static constexpr auto stores_reference_flag = std::uint32_t{1} << 30;
    static constexpr auto selection_flags = retained_flag | stores_reference_flag;
    static constexpr auto release_queue_index_mask = ~selection_flags;
    std::uint32_t state_{};
};

template <typename Owner, typename RuntimeType> struct ObservedItemCollection {
    using Selection = ItemSelection<ObservedItemCollection>;
    using SelectionMap = util::hash_dictionary<item_id_t, Selection>;

    ObservedItemCollection(Owner &owner, Reference reference, const RuntimeType &view_runtime_type,
                           const RuntimeType &runtime_type, const void *storage) noexcept
        : owner{&owner}, reference{reference}, view_runtime_type{&view_runtime_type}, runtime_type{&runtime_type},
          storage{storage} {}

    Owner *owner;
    Reference reference;
    const RuntimeType *view_runtime_type;
    const RuntimeType *runtime_type;
    const void *storage;
    SelectionMap items;
    bool observing_reference{};
};

template <typename Derived, typename RuntimeType> class ItemSelectionSubscriptions : public ReferenceProviderComponent {
  protected:
    using Collection = ObservedItemCollection<Derived, RuntimeType>;
    using Selection = typename Collection::Selection;

    ItemSelectionSubscriptions(ReferenceProvider &provider, allocator memory)
        : provider_{&provider}, collections_{&memory.memory_resource()}, release_queue_{&memory.memory_resource()},
          memory_{memory} {}

    ~ItemSelectionSubscriptions() override { assert(collections_.empty()); }

    Collection &require_collection(Reference reference, const RuntimeType &view_runtime_type,
                                   const RuntimeType &runtime_type, const void *storage) {
        if (auto *existing = find_collection(reference, view_runtime_type, runtime_type, storage)) {
            return *existing;
        }

        auto *result = memory_.new_object<Collection>(derived(), reference, view_runtime_type, runtime_type, storage);
        auto release = util::scope_exit{[&]() noexcept { release_collection(*result); }};
        collections_.push_back(result);
        auto remove_entry = util::scope_exit{[&]() noexcept { collections_.pop_back(); }};
        provider_->add_observer(reference, provider_->owner(), result, &notify_collection);
        result->observing_reference = true;

        remove_entry.release();
        release.release();
        return *result;
    }

    Selection &require_selection(Collection &collection, item_id_t id, bool stores_reference) {
        if (auto *entry = collection.items.find(id)) {
            assert(entry->stores_reference() == stores_reference);
            return *entry;
        }

        if (release_queue_.capacity() <= entry_count_) {
            release_queue_.reserve(std::max<std::size_t>(release_queue_.capacity() * 2, 8));
        }

        auto *value_item_reference =
            stores_reference ? nullptr : memory_.new_object<ItemReference>(collection.reference, id);
        auto release_item_reference = util::scope_exit{[&]() noexcept {
            if (value_item_reference != nullptr) {
                memory_.delete_object(value_item_reference);
            }
        }};

        auto [entry, inserted] = collection.items.try_emplace(id, memory_);
        assert(inserted);
        entry->collection = &collection;
        entry->selection.value_item_reference = value_item_reference;
        entry->id = id;
        entry->set_stores_reference(stores_reference);
        release_item_reference.release();
        ++entry_count_;
        return *entry;
    }

    bool refresh(Selection &entry, const ResolvedTimeSeries &resolved, EvaluationTime evaluation_time) {
        auto &collection = *entry.collection;
        auto value = Reference{};
        if (resolved.runtime_type == collection.view_runtime_type &&
            derived().retention_matches(collection, resolved) &&
            derived().contains_item(collection, resolved, entry.id)) {
            const auto item = collection.view_runtime_type->item(resolved.storage, entry.id);
            if (item.last_updated.valid()) {
                value = entry.stores_reference() ? item.runtime_type.reference(item.value)
                                                 : Reference{entry.selection.value_item_reference};
            }
        }
        if (value == entry.selected_reference) {
            return false;
        }

        entry.selected_reference = value;
        if (value) {
            entry.selection_time.publish(evaluation_time);
        } else if (entry.selection_time.valid()) {
            entry.selection_time.invalidate(evaluation_time);
        }
        return true;
    }

    template <typename Result>
    Result input(Selection &entry, const ResolvedTimeSeries &resolved,
                 const TimeSeriesRuntimeType &item_reference_runtime_type) {
        refresh(entry, resolved, provider_->owner().evaluation_time());
        queue_release(entry);

        return {&entry.selection_time, &item_reference_runtime_type, ReferenceBinding{&entry, binding_type()},
                entry.selection_time.valid() && entry.selection_time.time() == provider_->owner().evaluation_time()};
    }

    void release_all() noexcept {
        release_queue_.clear();
        for (auto *collection : collections_) {
            release_collection(*collection);
        }
        collections_.clear();
    }

    allocator &allocation() noexcept { return memory_; }

  public:
    void detach() noexcept final {
        provider_->remove_cycle_callback(this);
        for (auto *collection : collections_) {
            if (!collection->observing_reference) {
                continue;
            }
            provider_->remove_observer(collection->reference, provider_->owner(), collection);
            collection->observing_reference = false;
        }
    }

    void notify_item_bindings(Reference reference, item_id_t id, OutputChangeContext &context) final {
        auto resolved = ResolvedTimeSeries{};
        auto did_resolve = false;
        for (auto *collection : collections_) {
            if (collection->reference != reference) {
                continue;
            }
            if (auto *entry = collection->items.find(id)) {
                if (!did_resolve) {
                    resolved = ResolvedTimeSeries::resolve(reference);
                    did_resolve = true;
                }
                refresh_selection(context, *entry, resolved);
            }
        }
    }

    void destroy(allocator &memory) noexcept final { memory.delete_object(static_cast<Derived *>(this)); }

    void add_binding_subscriber(Selection &entry, RuntimeGraph &graph, std::uint32_t input_offset,
                                std::size_t notification_data, std::uint32_t node_index,
                                InputNotificationFunction notification) {
        remove_from_release_queue(entry);
        if (entry.stores_reference()) {
            auto &subscribers = DynamicSubscribers::ensure(entry.selection.reference_item_subscribers, memory_);
            subscribers.add(graph, input_offset, notification_data, node_index, false, false, notification);
            return;
        }

        ReferenceProvider::add_subscriber(Reference{entry.selection.value_item_reference}, graph, input_offset,
                                          notification_data, node_index, false, false, notification);
    }

    void remove_binding_subscriber(Selection &entry, RuntimeGraph &graph, std::size_t input_offset) noexcept {
        if (entry.stores_reference()) {
            entry.selection.reference_item_subscribers->remove(graph, input_offset);
        } else {
            ReferenceProvider::remove_subscriber(Reference{entry.selection.value_item_reference}, graph, input_offset);
        }
        if (!has_active_subscribers(entry)) {
            queue_release(entry);
        }
    }

  private:
    class BindingType final : public ReferenceBindingType {
      public:
        const Reference &value(const void *data) const noexcept override {
            return static_cast<const Selection *>(data)->selected_reference;
        }

        void subscribe(void *data, RuntimeGraph &graph, std::uint32_t input_offset, std::size_t notification_data,
                       std::uint32_t node_index, InputNotificationFunction notification) const override {
            auto &entry = *static_cast<Selection *>(data);
            entry.collection->owner->add_binding_subscriber(entry, graph, input_offset, notification_data, node_index,
                                                            notification);
        }

        void unsubscribe(void *data, RuntimeGraph &graph, std::size_t input_offset) const noexcept override {
            auto &entry = *static_cast<Selection *>(data);
            entry.collection->owner->remove_binding_subscriber(entry, graph, input_offset);
        }
    };

    static const ReferenceBindingType &binding_type() noexcept {
        static const auto result = BindingType{};
        return result;
    }

    Derived &derived() noexcept { return static_cast<Derived &>(*this); }

    Collection *find_collection(Reference reference, const RuntimeType &view_runtime_type,
                                const RuntimeType &runtime_type, const void *storage) const {
        const auto found = std::ranges::find_if(collections_, [&](const auto *candidate) {
            return candidate->reference == reference && candidate->view_runtime_type == &view_runtime_type &&
                   candidate->runtime_type == &runtime_type && candidate->storage == storage;
        });
        return found == collections_.end() ? nullptr : *found;
    }

    void queue_release(Selection &entry) noexcept {
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

    void remove_from_release_queue(Selection &entry) noexcept {
        if (!entry.queued_for_release()) {
            return;
        }

        const auto index = entry.release_queue_index();
        auto *replacement = release_queue_.back();
        release_queue_[index] = replacement;
        replacement->set_release_queue_index(static_cast<std::uint32_t>(index));
        release_queue_.pop_back();
        entry.clear_release_queue_index();
    }

    bool has_active_subscribers(const Selection &entry) const noexcept {
        if (entry.stores_reference()) {
            return entry.selection.reference_item_subscribers != nullptr &&
                   !entry.selection.reference_item_subscribers->empty();
        }
        return provider_->item_reference_has_subscribers(*entry.selection.value_item_reference);
    }

    void erase_selection(Selection &entry) noexcept {
        auto &collection = *entry.collection;
        assert(!has_active_subscribers(entry));
        derived().release_item_retention(collection, entry);

        if (!entry.stores_reference()) {
            if (!provider_->release_item_reference(*entry.selection.value_item_reference)) {
                queue_release(entry);
                return;
            }
            memory_.delete_object(entry.selection.value_item_reference);
        } else {
            DynamicSubscribers::release(entry.selection.reference_item_subscribers);
        }

        const auto id = entry.id;
        collection.items.erase_with(
            id, memory_, [](item_id_t *) noexcept {}, [](Selection *value) noexcept { std::destroy_at(value); });
        --entry_count_;
    }

    void erase_collection(Collection &collection) noexcept {
        const auto position = std::ranges::find(collections_, &collection);
        *position = collections_.back();
        collections_.pop_back();
        release_collection(collection);
    }

    void release_collection(Collection &collection) noexcept {
        if (collection.observing_reference) {
            provider_->remove_observer(collection.reference, provider_->owner(), &collection);
        }
        for (const auto item : collection.items) {
            auto &entry = item.value;
            derived().release_item_retention(collection, entry);
            if (!entry.stores_reference()) {
                memory_.delete_object(entry.selection.value_item_reference);
            } else {
                DynamicSubscribers::release(entry.selection.reference_item_subscribers);
            }
        }
        collection.items.release_with(
            memory_, [](item_id_t *) noexcept {}, [](Selection *entry) noexcept { std::destroy_at(entry); });
        memory_.delete_object(&collection);
    }

    static void notify_collection(OutputChangeContext &context, void *value, OutputChange change) {
        auto &collection = *static_cast<Collection *>(value);
        collection.owner->notify(context, collection, change);
    }

    void notify(OutputChangeContext &context, Collection &collection, OutputChange change) {
        if (change == OutputChange::invalidation) {
            invalidate_all(context, collection);
            release_all_item_retention(collection);
            return;
        }

        const auto resolved = ResolvedTimeSeries::resolve(collection.reference);
        if (resolved.runtime_type != collection.view_runtime_type ||
            !derived().retention_matches(collection, resolved)) {
            invalidate_all(context, collection);
            release_all_item_retention(collection);
            return;
        }

        if (change == OutputChange::update) {
            notify_updated_items(context, collection, resolved);
            return;
        }

        for (const auto item : collection.items) {
            refresh_selection(context, item.value, resolved);
        }
    }

    void invalidate_all(OutputChangeContext &context, Collection &collection) {
        for (const auto item : collection.items) {
            auto &entry = item.value;
            if (!entry.selected_reference) {
                continue;
            }
            entry.selected_reference = {};
            if (entry.selection_time.valid()) {
                entry.selection_time.invalidate(context.evaluation_time());
            }
            notify_selection(context, entry, OutputChange::invalidation);
        }
    }

    void release_all_item_retention(Collection &collection) noexcept {
        for (const auto item : collection.items) {
            derived().release_item_retention(collection, item.value);
        }
    }

    void notify_updated_items(OutputChangeContext &context, Collection &collection, ResolvedTimeSeries resolved) {
        struct Callback {
            ItemSelectionSubscriptions *owner;
            Collection *collection;
            OutputChangeContext *context;
            const ResolvedTimeSeries *resolved;
        } callback{this, &collection, &context, &resolved};

        collection.view_runtime_type->for_each_updated_item(
            *resolved.last_updated, resolved.storage, &callback, [](void *value, item_id_t id) {
                auto &callback = *static_cast<Callback *>(value);
                if (auto *entry = callback.collection->items.find(id)) {
                    callback.owner->refresh_selection(*callback.context, *entry, *callback.resolved);
                }
            });
    }

    void refresh_selection(OutputChangeContext &context, Selection &entry, const ResolvedTimeSeries &resolved) {
        const auto was_valid = static_cast<bool>(entry.selected_reference);
        const auto value_changed = refresh(entry, resolved, context.evaluation_time());
        const auto is_valid = static_cast<bool>(entry.selected_reference);

        if (entry.stores_reference() && !value_changed) {
            return;
        }
        if (!is_valid) {
            if (was_valid) {
                notify_selection(context, entry, OutputChange::invalidation);
            }
            return;
        }

        notify_selection(context, entry, was_valid ? OutputChange::update : OutputChange::revalidation);
    }

    static void notify_selection(OutputChangeContext &context, Selection &entry, OutputChange change) {
        if (entry.stores_reference()) {
            context.notify_subscribers(entry.selection.reference_item_subscribers, change);
        } else {
            ReferenceProvider::notify(Reference{entry.selection.value_item_reference}, context, change);
        }
    }

    static void release_queued_entries(void *value, EvaluationContext &) noexcept {
        static_cast<ItemSelectionSubscriptions *>(value)->release_queued_entries();
    }

    void release_queued_entries() noexcept {
        cycle_callback_registered_ = false;
        const auto release_count = release_queue_.size();
        for (auto index = std::size_t{}; index < release_count; ++index) {
            auto *entry = release_queue_.back();
            release_queue_.pop_back();
            entry->clear_release_queue_index();
            if (!has_active_subscribers(*entry)) {
                erase_selection(*entry);
            }
        }

        auto index = std::size_t{};
        while (index < collections_.size()) {
            if (collections_[index]->items.empty()) {
                erase_collection(*collections_[index]);
            } else {
                ++index;
            }
        }
    }

    ReferenceProvider *provider_;
    std::pmr::vector<Collection *> collections_;
    std::pmr::vector<Selection *> release_queue_;
    std::size_t entry_count_{};
    allocator memory_;
    bool cycle_callback_registered_{};
};

} // namespace bg::details
