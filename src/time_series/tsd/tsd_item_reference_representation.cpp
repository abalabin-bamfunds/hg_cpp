#include "tsd_reference_representation.hpp"

#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {
namespace {

class TimeSeriesDictionaryReferenceRepresentation;

class RoutedReferenceRuntimeType final : public TimeSeriesReferenceRuntimeType {
  public:
    explicit RoutedReferenceRuntimeType(const TimeSeriesReferenceType &type) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(ItemReference), alignof(ItemReference)} {}

    Reference reference(const void *value) const override {
        return Reference{const_cast<ItemReference *>(static_cast<const ItemReference *>(value))};
    }
};

class TimeSeriesDictionaryReferenceRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    TimeSeriesDictionaryReferenceRuntimeType(const TimeSeriesDictionaryType &type,
                                             const TimeSeriesDictionaryRuntimeType &base,
                                             const TimeSeriesRuntimeType &item,
                                             TimeSeriesDictionaryReferenceRepresentation &representation)
        : TimeSeriesDictionaryRuntimeType{type, base.key_runtime_type(), item}, representation_{&representation} {}

    ConstItem item(const void *value, item_id_t id) const override;
    MutableItem item(void *, item_id_t) const override;
    bool item_updated(const EvaluationTime &last_updated, const void *value, item_id_t id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified(const EvaluationTime &, const void *, void *, ItemIdFunction) const override {}
    bool item_modified(const EvaluationTime &, const void *, item_id_t) const noexcept override { return false; }
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

  private:
    TimeSeriesDictionaryReferenceRepresentation *representation_;
};

struct ReferenceItemStorage {
    ReferenceItemStorage(Reference parent, item_id_t id, EvaluationTime initial_last_updated)
        : reference{parent, id}, last_updated{initial_last_updated} {}

    ItemReference reference;
    EvaluationTime last_updated;
};

static_assert(sizeof(ReferenceItemStorage) == 4 * sizeof(void *));

struct RetiredItem {
    item_id_t id;
    EvaluationTime time;
};

class TimeSeriesDictionaryReferenceRepresentation final : public ReferenceRepresentation {
  public:
    TimeSeriesDictionaryReferenceRepresentation(ReferenceProvider &provider, Reference base_reference,
                                                const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                const TimeSeriesRuntimeType &item_runtime_type)
        : ReferenceRepresentation{base_reference}, provider_{&provider}, base_runtime_type_{&base_runtime_type},
          item_runtime_type_{static_cast<const TimeSeriesReferenceType &>(item_runtime_type.type())},
          runtime_type_{type, base_runtime_type, item_runtime_type_, *this},
          last_updated_{*resolve_reference(base_reference).last_updated}, retired_{&provider.memory()} {
        const auto base_time_series = resolve_reference(base_reference);
        const auto *storage = base_time_series.storage;
        base_runtime_type.for_each_item_id(storage, this, [](void *context, item_id_t id) {
            auto &self = *static_cast<TimeSeriesDictionaryReferenceRepresentation *>(context);
            self.add_item(id, self.last_updated_);
        });
    }

    ResolvedTimeSeries resolve() const override {
        return {&last_updated_, resolve_reference(base_reference()).storage, &runtime_type_};
    }

    void notify(OutputChangeContext &context, OutputChange change) override {
        release_retired(context.evaluation_time());
        provider_->begin_item_notification_batch();
        auto cancel_items = util::scope_exit{[&]() noexcept { provider_->cancel_item_notification_batch(); }};

        if (change == OutputChange::invalidation) {
            invalidate(context);
            provider_->publish_item_notification_batch(context);
            cancel_items.release();
            return;
        }
        if (last_updated_ <= MIN_DT) {
            revalidate(context);
            provider_->publish_item_notification_batch(context);
            cancel_items.release();
            return;
        }

        const auto base_time_series = resolve_reference(base_reference());
        const auto input_time = *base_time_series.last_updated;
        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputChangeContext *context;
            EvaluationTime input_time;
        } callback{this, &context, input_time};

        base_runtime_type_->for_each_added(input_time, base_time_series.storage, &callback,
                                           [](void *value, item_id_t id) {
                                               auto &callback = *static_cast<Callback *>(value);
                                               callback.representation->add_item(id, callback.input_time);
                                           });
        base_runtime_type_->for_each_modified(input_time, base_time_series.storage, &callback,
                                              [](void *value, item_id_t id) {
                                                  auto &callback = *static_cast<Callback *>(value);
                                                  callback.representation->update_item(*callback.context, id);
                                              });
        base_runtime_type_->for_each_removed(input_time, base_time_series.storage, &callback,
                                             [](void *value, item_id_t id) {
                                                 auto &callback = *static_cast<Callback *>(value);
                                                 callback.representation->remove_item(*callback.context, id);
                                             });

        if (!base_runtime_type_->item_updated(input_time, base_time_series.storage, key_set_item_id)) {
            provider_->publish_item_notification_batch(context);
            cancel_items.release();
            return;
        }

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = context.evaluation_time();
        notify_reference(reference(), context, was_valid ? OutputChange::update : OutputChange::revalidation);
        provider_->publish_item_notification_batch(context);
        cancel_items.release();
    }

    void destroy(std::pmr::memory_resource &memory) noexcept override {
        items_.release(positions_, memory);
        positions_.release(memory);
        item_valid_.release(memory);
        std::destroy_at(this);
        memory.deallocate(this, sizeof(TimeSeriesDictionaryReferenceRepresentation),
                          alignof(TimeSeriesDictionaryReferenceRepresentation));
    }

    const ReferenceItemStorage &item(item_id_t id) const noexcept { return items_[id]; }

  private:
    void invalidate(OutputChangeContext &context) {
        if (last_updated_ <= MIN_DT) {
            return;
        }

        for (const auto id : item_valid_) {
            provider_->notify(items_[id].reference.subscriptions(), context, OutputChange::invalidation);
        }
        item_valid_.clear();
        last_updated_ = MIN_DT;
        notify_reference(reference(), context, OutputChange::invalidation);
    }

    void revalidate(OutputChangeContext &context) {
        auto current = util::index<>{};
        auto release_current = util::scope_exit{[&]() noexcept { current.release(provider_->memory()); }};
        const auto base_time_series = resolve_reference(base_reference());

        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputChangeContext *context;
            util::index<> *current;
        } callback{this, &context, &current};
        base_runtime_type_->for_each_item_id(base_time_series.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            callback.current->set(id, self.provider_->memory());
            if (!self.positions_.contains(id)) {
                self.add_item(id, callback.context->evaluation_time());
                return;
            }

            self.restore_item(id);
            const auto item = self.base_runtime_type_->item(resolve_reference(self.base_reference()).storage, id);
            if (item.last_updated <= MIN_DT) {
                self.item_valid_.reset(id);
                return;
            }

            self.item_valid_.set(id, self.provider_->memory());
            self.provider_->notify(self.items_[id].reference.subscriptions(), *callback.context,
                                   OutputChange::revalidation);
        });

        for (const auto id : positions_) {
            if (!current.contains(id)) {
                retire_item(id, context.evaluation_time());
            }
        }

        last_updated_ = context.evaluation_time();
        notify_reference(reference(), context, OutputChange::revalidation);
    }

    void add_item(item_id_t id, EvaluationTime reference_time) {
        if (positions_.contains(id)) {
            restore_item(id);
            return;
        }

        auto &memory = provider_->memory();
        const auto base_time_series = resolve_reference(base_reference());
        const auto item = base_runtime_type_->item(base_time_series.storage, id);
        items_.emplace_at(id, memory, base_reference(), id, reference_time);
        positions_.set(id, memory);
        if (item.last_updated > MIN_DT) {
            item_valid_.set(id, memory);
        }
    }

    void update_item(OutputChangeContext &context, item_id_t id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        const auto base_time_series = resolve_reference(base_reference());
        const auto item_time_series = base_runtime_type_->item(base_time_series.storage, id);
        const auto now_valid = item_time_series.last_updated > MIN_DT;
        const auto was_valid = item_valid_.contains(id);
        if (now_valid) {
            item_valid_.set(id, provider_->memory());
            provider_->notify(item.reference.subscriptions(), context,
                              was_valid ? OutputChange::update : OutputChange::revalidation);
            return;
        }
        if (was_valid) {
            item_valid_.reset(id);
            provider_->notify(item.reference.subscriptions(), context, OutputChange::invalidation);
        }
    }

    void remove_item(OutputChangeContext &context, item_id_t id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        if (item_valid_.reset(id)) {
            provider_->notify(item.reference.subscriptions(), context, OutputChange::invalidation);
        }
        retire_item(id, context.evaluation_time());
    }

    void retire_item(item_id_t id, EvaluationTime time) {
        const auto existing = std::ranges::find(retired_, id, &RetiredItem::id);
        if (existing == retired_.end()) {
            retired_.push_back({id, time});
        }
    }

    void restore_item(item_id_t id) {
        const auto retired = std::ranges::find(retired_, id, &RetiredItem::id);
        if (retired != retired_.end()) {
            retired_.erase(retired);
        }
    }

    void release_retired(EvaluationTime evaluation_time) noexcept {
        auto &memory = provider_->memory();
        auto retained = retired_.begin();
        for (const auto item : retired_) {
            if (item.time >= evaluation_time) {
                *retained++ = item;
                continue;
            }
            if (!provider_->release_item_reference(items_[item.id].reference)) {
                *retained++ = item;
                continue;
            }
            items_.erase_at(item.id, memory);
            positions_.reset(item.id);
        }
        retired_.erase(retained, retired_.end());
    }

    ReferenceProvider *provider_;
    const TimeSeriesDictionaryRuntimeType *base_runtime_type_;
    RoutedReferenceRuntimeType item_runtime_type_;
    TimeSeriesDictionaryReferenceRuntimeType runtime_type_;
    EvaluationTime last_updated_;
    util::occupancy_index positions_;
    util::index<> item_valid_;
    util::sparse_bunch<ReferenceItemStorage> items_;
    std::pmr::vector<RetiredItem> retired_;
};

TimeSeriesRuntimeType::ConstItem TimeSeriesDictionaryReferenceRuntimeType::item(const void *value, item_id_t id) const {
    if (id == key_set_item_id) {
        return TimeSeriesDictionaryRuntimeType::item(value, id);
    }

    const auto &item = representation_->item(id);
    return {item_runtime_type(), item.last_updated, &item.reference};
}

TimeSeriesRuntimeType::MutableItem TimeSeriesDictionaryReferenceRuntimeType::item(void *, item_id_t) const {
    throw std::logic_error{"a dictionary reference representation is immutable"};
}

bool TimeSeriesDictionaryReferenceRuntimeType::item_updated(const EvaluationTime &last_updated, const void *value,
                                                            item_id_t id) const noexcept {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    if (last_updated <= MIN_DT || storage.change_time() != last_updated) {
        return false;
    }
    if (id == key_set_item_id) {
        return storage.key_storage().last_updated == last_updated;
    }

    const auto &keys = storage.key_storage().value;
    return keys.added().contains(id) || keys.removed().contains(id);
}

void TimeSeriesDictionaryReferenceRuntimeType::for_each_updated_item(const EvaluationTime &last_updated,
                                                                     const void *value, void *context,
                                                                     ItemIdFunction function) const {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    if (last_updated <= MIN_DT || storage.change_time() != last_updated) {
        return;
    }

    const auto &keys = storage.key_storage().value;
    if (storage.key_storage().last_updated == last_updated) {
        function(context, key_set_item_id);
    }
    for (const auto id : keys.added()) {
        function(context, id);
    }
    for (const auto id : keys.removed()) {
        function(context, id);
    }
}

void TimeSeriesDictionaryReferenceRuntimeType::for_each_modified_item(const EvaluationTime &last_updated,
                                                                      const void *value, EvaluationTime evaluation_time,
                                                                      void *context,
                                                                      ModifiedItemFunction function) const {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    const auto &keys = storage.key_storage().value;
    if (last_updated <= MIN_DT || storage.change_time() != evaluation_time) {
        return;
    }

    if (storage.key_storage().last_updated == evaluation_time) {
        const auto key_set = TimeSeriesDictionaryRuntimeType::item(value, key_set_item_id);
        function(context, {key_set_item_id, &key_set.runtime_type, &storage.key_storage().last_updated,
                           &storage.key_storage().value});
    }
    for (const auto id : keys.added()) {
        const auto &item = representation_->item(id);
        function(context, {id, &item_runtime_type(), &item.last_updated, &item.reference});
    }
    for (const auto id : keys.removed()) {
        const auto &item = representation_->item(id);
        function(context, {id, &item_runtime_type(), &MIN_DT, &item.reference});
    }
}

} // namespace

ReferenceRepresentation *make_tsd_item_reference_representation(
    ReferenceProvider &provider, Reference base_reference, const TimeSeriesDictionaryType &type,
    const TimeSeriesDictionaryRuntimeType &base_runtime_type, const TimeSeriesRuntimeType &item_runtime_type,
    std::pmr::memory_resource &memory) {
    auto *storage = memory.allocate(sizeof(TimeSeriesDictionaryReferenceRepresentation),
                                    alignof(TimeSeriesDictionaryReferenceRepresentation));
    auto release = util::scope_exit{[&]() noexcept {
        memory.deallocate(storage, sizeof(TimeSeriesDictionaryReferenceRepresentation),
                          alignof(TimeSeriesDictionaryReferenceRepresentation));
    }};
    auto *result = std::construct_at(static_cast<TimeSeriesDictionaryReferenceRepresentation *>(storage), provider,
                                     base_reference, type, base_runtime_type, item_runtime_type);
    release.release();
    return result;
}

} // namespace bg::details
