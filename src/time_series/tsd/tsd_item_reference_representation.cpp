#include "tsd_reference_representation.hpp"

#include <bg/allocator.hpp>
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

struct ReferenceItemStorage {
    ReferenceItemStorage(Reference parent, item_id_t id, EvaluationTime initial_last_updated)
        : reference{parent, id} {
        last_updated.publish(initial_last_updated);
    }

    ItemReference reference;
    TimeSeriesState last_updated;
};

static_assert(sizeof(ReferenceItemStorage) == 4 * sizeof(void *));

struct RetiredItem {
    item_id_t id;
    EvaluationTime time;
};

class TimeSeriesDictionaryReferenceRepresentation final : public TimeSeriesDictionaryRepresentation {
  public:
    TimeSeriesDictionaryReferenceRepresentation(ReferenceProvider &provider, Reference base_reference,
                                                const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                const TimeSeriesRuntimeType &item_runtime_type)
        : TimeSeriesDictionaryRepresentation{base_reference, type},
          provider_{&provider}, item_runtime_type_{&item_runtime_type},
          retired_{&provider.allocation().memory_resource()} {
        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference);
        last_updated_ = base_time_series.last_updated;
        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            const ResolvedTimeSeries *base_time_series;
        } callback{this, &base_time_series};
        if (base_time_series.storage != nullptr) {
            base_runtime_type.for_each_item_id(base_time_series.storage, &callback, [](void *context, item_id_t id) {
                auto &callback = *static_cast<Callback *>(context);
                callback.representation->add_item(id, callback.representation->last_updated_.time(),
                                                 *callback.base_time_series);
            });
        }
    }

    ResolvedTimeSeries resolve() const override {
        return {last_updated_, this, &dictionary_runtime_type()};
    }

    ResolvedTimeSeries base_time_series() const override { return ResolvedTimeSeries::resolve(base_reference()); }

    TimeSeriesRuntimeType::ConstItem item(item_id_t id) const override {
        const auto &item = items_[id];
        return {*item_runtime_type_, item.last_updated, &item.reference};
    }

    bool item_updated(const TimeSeriesState &last_updated, item_id_t id) const noexcept override {
        if (!last_updated.valid()) {
            return false;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return false;
        }
        const auto &runtime_type = base_runtime_type(base);
        if (id == key_set_item_id) {
            return runtime_type.item_updated(last_updated, base.storage, id);
        }
        return runtime_type.next_item_id(last_updated, base.storage,
                                         TimeSeriesDictionaryRuntimeType::ItemRange::added, id) == id ||
               runtime_type.next_item_id(last_updated, base.storage,
                                         TimeSeriesDictionaryRuntimeType::ItemRange::removed, id) == id;
    }

    bool is_updated_item(const TimeSeriesState &, item_id_t) const noexcept override { return false; }

    bool is_modified_item(const TimeSeriesState &last_updated, item_id_t id) const noexcept override {
        const auto base = base_time_series();
        return base.storage != nullptr &&
               base_runtime_type(base).item_in_range(last_updated, base.storage,
                                                     TimeSeriesDictionaryRuntimeType::ItemRange::added, id);
    }

    item_id_t next_updated_item_id(const TimeSeriesState &, item_id_t) const noexcept override {
        return util::occupancy_index::npos;
    }

    item_id_t next_modified_item_id(const TimeSeriesState &last_updated, item_id_t start) const noexcept override {
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return util::occupancy_index::npos;
        }
        return base_runtime_type(base).next_item_id(last_updated, base.storage,
                                                    TimeSeriesDictionaryRuntimeType::ItemRange::added, start);
    }

    void for_each_updated(const TimeSeriesState &, void *, TimeSeriesRuntimeType::ItemIdFunction) const override {}

    void for_each_modified(const TimeSeriesState &last_updated, void *context,
                           TimeSeriesRuntimeType::ItemIdFunction function) const override {
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return;
        }
        base_runtime_type(base).for_each_added(last_updated, base.storage, context, function);
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, void *context,
                               TimeSeriesRuntimeType::ItemIdFunction function) const override {
        if (!last_updated.valid()) {
            return;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return;
        }
        const auto &runtime_type = base_runtime_type(base);
        if (runtime_type.item_updated(last_updated, base.storage, key_set_item_id)) {
            function(context, key_set_item_id);
        }
        for (auto id = runtime_type.next_item_id(last_updated, base.storage,
                                                 TimeSeriesDictionaryRuntimeType::ItemRange::added, 0);
             id != util::occupancy_index::npos;
             id = runtime_type.next_item_id(last_updated, base.storage,
                                            TimeSeriesDictionaryRuntimeType::ItemRange::added, id + 1)) {
            function(context, id);
        }
        for (auto id = runtime_type.next_item_id(last_updated, base.storage,
                                                 TimeSeriesDictionaryRuntimeType::ItemRange::removed, 0);
             id != util::occupancy_index::npos;
             id = runtime_type.next_item_id(last_updated, base.storage,
                                            TimeSeriesDictionaryRuntimeType::ItemRange::removed, id + 1)) {
            function(context, id);
        }
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, EvaluationTime evaluation_time, void *context,
                                TimeSeriesRuntimeType::ModifiedItemFunction function) const override {
        const auto base = base_time_series();
        if (base.storage == nullptr || !last_updated.valid()) {
            return;
        }
        const auto &runtime_type = base_runtime_type(base);
        const auto key_set = runtime_type.item(base.storage, key_set_item_id);
        if (key_set.last_updated.time() == evaluation_time) {
            function(context, {key_set_item_id, &key_set.runtime_type, &key_set.last_updated, key_set.value});
        }
        for (auto id = runtime_type.next_item_id(last_updated, base.storage,
                                                 TimeSeriesDictionaryRuntimeType::ItemRange::added, 0);
             id != util::occupancy_index::npos;
             id = runtime_type.next_item_id(last_updated, base.storage,
                                            TimeSeriesDictionaryRuntimeType::ItemRange::added, id + 1)) {
            const auto value = item(id);
            function(context, {id, &value.runtime_type, &value.last_updated, value.value});
        }
        for (auto id = runtime_type.next_item_id(last_updated, base.storage,
                                                 TimeSeriesDictionaryRuntimeType::ItemRange::removed, 0);
             id != util::occupancy_index::npos;
             id = runtime_type.next_item_id(last_updated, base.storage,
                                            TimeSeriesDictionaryRuntimeType::ItemRange::removed, id + 1)) {
            const auto value = item(id);
            function(context, {id, &value.runtime_type, &initial_time_series_state, value.value});
        }
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
        if (!last_updated_.valid()) {
            revalidate(context);
            provider_->publish_item_notification_batch(context);
            cancel_items.release();
            return;
        }

        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference());
        const auto input_time = base_time_series.last_updated.time();
        const auto &input_runtime_type = base_runtime_type(base_time_series);
        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputChangeContext *context;
            const ResolvedTimeSeries *base_time_series;
            EvaluationTime input_time;
        } callback{this, &context, &base_time_series, input_time};

        input_runtime_type.for_each_added(base_time_series.last_updated, base_time_series.storage, &callback,
                                          [](void *value, item_id_t id) {
                                              auto &callback = *static_cast<Callback *>(value);
                                              callback.representation->add_item(id, callback.input_time,
                                                                                *callback.base_time_series);
                                          });
        input_runtime_type.for_each_updated(base_time_series.last_updated, base_time_series.storage, &callback,
                                             [](void *value, item_id_t id) {
                                                 auto &callback = *static_cast<Callback *>(value);
                                                 callback.representation->update_item(
                                                     *callback.context, id, *callback.base_time_series);
                                             });
        input_runtime_type.for_each_removed(base_time_series.last_updated, base_time_series.storage, &callback,
                                            [](void *value, item_id_t id) {
                                                auto &callback = *static_cast<Callback *>(value);
                                                callback.representation->remove_item(*callback.context, id);
                                            });

        if (!input_runtime_type.item_updated(base_time_series.last_updated, base_time_series.storage,
                                             key_set_item_id)) {
            provider_->publish_item_notification_batch(context);
            cancel_items.release();
            return;
        }

        const auto was_valid = last_updated_.valid();
        last_updated_.publish(context.evaluation_time());
        ReferenceProvider::notify(reference(), context,
                                  was_valid ? OutputChange::update : OutputChange::revalidation);
        provider_->publish_item_notification_batch(context);
        cancel_items.release();
    }

    void destroy(allocator &memory) noexcept override {
        items_.release(positions_, memory);
        positions_.release(memory);
        item_valid_.release(memory);
        memory.delete_object(this);
    }

    const ReferenceItemStorage &reference_item(item_id_t id) const noexcept { return items_[id]; }

  private:
    void invalidate(OutputChangeContext &context) {
        if (!last_updated_.valid()) {
            return;
        }

        for (const auto id : item_valid_) {
            provider_->notify_subscriptions(items_[id].reference.subscriptions(), context, OutputChange::invalidation);
        }
        item_valid_.clear();
        last_updated_.invalidate(context.evaluation_time());
        ReferenceProvider::notify(reference(), context, OutputChange::invalidation);
    }

    void revalidate(OutputChangeContext &context) {
        auto current = util::index<>{};
        auto release_current = util::scope_exit{[&]() noexcept { current.release(provider_->allocation()); }};
        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference());

        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputChangeContext *context;
            util::index<> *current;
            const ResolvedTimeSeries *base_time_series;
        } callback{this, &context, &current, &base_time_series};
        base_runtime_type(base_time_series).for_each_item_id(base_time_series.storage, &callback,
                                                               [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            callback.current->set(id, self.provider_->allocation());
            if (!self.positions_.contains(id)) {
                self.add_item(id, callback.context->evaluation_time(), *callback.base_time_series);
                return;
            }

            self.restore_item(id);
            const auto item = self.base_runtime_type(*callback.base_time_series)
                                  .item(callback.base_time_series->storage, id);
            if (!item.last_updated.valid()) {
                self.item_valid_.reset(id);
                return;
            }

            self.item_valid_.set(id, self.provider_->allocation());
            self.provider_->notify_subscriptions(self.items_[id].reference.subscriptions(), *callback.context,
                                                 OutputChange::revalidation);
        });

        for (const auto id : positions_) {
            if (!current.contains(id)) {
                retire_item(id, context.evaluation_time());
            }
        }

        last_updated_.publish(context.evaluation_time());
        ReferenceProvider::notify(reference(), context, OutputChange::revalidation);
    }

    void add_item(item_id_t id, EvaluationTime reference_time, const ResolvedTimeSeries &base_time_series) {
        if (positions_.contains(id)) {
            restore_item(id);
            return;
        }

        auto &memory = provider_->allocation();
        const auto item = base_runtime_type(base_time_series).item(base_time_series.storage, id);
        items_.emplace_at(id, memory, base_reference(), id, reference_time);
        positions_.set(id, memory);
        if (item.last_updated.valid()) {
            item_valid_.set(id, memory);
        }
    }

    void update_item(OutputChangeContext &context, item_id_t id, const ResolvedTimeSeries &base_time_series) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        const auto item_time_series = base_runtime_type(base_time_series).item(base_time_series.storage, id);
        const auto now_valid = item_time_series.last_updated.valid();
        const auto was_valid = item_valid_.contains(id);
        if (now_valid) {
            item_valid_.set(id, provider_->allocation());
            provider_->notify_subscriptions(item.reference.subscriptions(), context,
                                            was_valid ? OutputChange::update : OutputChange::revalidation);
            return;
        }
        if (was_valid) {
            item_valid_.reset(id);
            provider_->notify_subscriptions(item.reference.subscriptions(), context, OutputChange::invalidation);
        }
    }

    void remove_item(OutputChangeContext &context, item_id_t id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        if (item_valid_.reset(id)) {
            provider_->notify_subscriptions(item.reference.subscriptions(), context, OutputChange::invalidation);
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
        auto &memory = provider_->allocation();
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
    const TimeSeriesRuntimeType *item_runtime_type_;
    TimeSeriesState last_updated_;
    util::occupancy_index positions_;
    util::index<> item_valid_;
    util::sparse_bunch<ReferenceItemStorage> items_;
    std::pmr::vector<RetiredItem> retired_;

    const TimeSeriesDictionaryRuntimeType &base_runtime_type(const ResolvedTimeSeries &base) const noexcept {
        return static_cast<const TimeSeriesDictionaryRuntimeType &>(*base.runtime_type);
    }
};

} // namespace

ReferenceRepresentation *make_tsd_item_reference_representation(
    ReferenceProvider &provider, Reference base_reference, const TimeSeriesDictionaryType &type,
    const TimeSeriesDictionaryRuntimeType &base_runtime_type, const TimeSeriesRuntimeType &item_runtime_type,
    allocator &memory) {
    return memory.new_object<TimeSeriesDictionaryReferenceRepresentation>(
        provider, base_reference, type, base_runtime_type, item_runtime_type);
}

} // namespace bg::details
