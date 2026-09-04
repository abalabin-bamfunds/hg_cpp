#include <bg/time_series/tsd/tsd_runtime_type.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <array>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>

namespace bg::details {
namespace {

class TimeSeriesDictionaryReferenceRepresentation;

class TimeSeriesDictionaryReferenceRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    TimeSeriesDictionaryReferenceRuntimeType(const TimeSeriesDictionaryType &type,
                                             const TimeSeriesDictionaryRuntimeType &source,
                                             const TimeSeriesRuntimeType &item,
                                             TimeSeriesDictionaryReferenceRepresentation &representation)
        : TimeSeriesDictionaryRuntimeType{type, source.key_runtime_type(), item}, representation_{&representation} {}

    [[nodiscard]] ConstItem item(const void *value, TimeSeriesItemId id) const override;
    [[nodiscard]] MutableItem item(void *, TimeSeriesItemId) const override;
    [[nodiscard]] bool item_updated(const EvaluationTime &last_updated, const void *value,
                                    TimeSeriesItemId id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified(const EvaluationTime &, const void *, void *, ItemIdFunction) const override {}
    [[nodiscard]] bool item_modified(const EvaluationTime &, const void *, TimeSeriesItemId) const noexcept override {
        return false;
    }
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

  private:
    TimeSeriesDictionaryReferenceRepresentation *representation_;
};

struct ItemReference {
    ItemReference(ReferenceProvider &provider, TimeSeriesRuntimeType::ConstItem target,
                  EvaluationTime reference_time) noexcept
        : reference{&provider, &target.last_updated, target.value, &target.runtime_type, nullptr},
          value{&reference}, last_updated{reference_time}, target_was_valid{target.last_updated > MIN_DT} {}

    ReferenceStorage reference;
    ReferenceStorage *value;
    EvaluationTime last_updated;
    bool target_was_valid;
};

class TimeSeriesDictionaryReferenceRepresentation final : public ReferenceRepresentation {
  public:
    TimeSeriesDictionaryReferenceRepresentation(ReferenceProvider &provider, ReferenceStorage &source,
                                                const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &source_runtime_type,
                                                const TimeSeriesRuntimeType &item_runtime_type)
        : source_{&source}, source_runtime_type_{&source_runtime_type},
          runtime_type_{type, source_runtime_type, item_runtime_type, *this},
          last_updated_{*source.target_last_updated},
          reference_{&provider, &last_updated_, source.target_value, &runtime_type_, nullptr},
          retired_{&provider.memory()} {
        const auto *storage = source.target_value;
        source_runtime_type.for_each_item_id(storage, this, [](void *context, TimeSeriesItemId id) {
            auto &self = *static_cast<TimeSeriesDictionaryReferenceRepresentation *>(context);
            self.add_item(id, self.last_updated_);
        });
    }

    [[nodiscard]] const ReferenceStorage &source() const noexcept override { return *source_; }
    [[nodiscard]] ReferenceStorage &reference() noexcept override { return reference_; }

    void source_changed(OutputNotificationContext &context, OutputChange) override {
        release_retired();

        const auto source_time = *source_->target_last_updated;
        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputNotificationContext *context;
            EvaluationTime source_time;
        } callback{this, &context, source_time};

        source_runtime_type_->for_each_added(source_time, source_->target_value, &callback,
                                             [](void *value, TimeSeriesItemId id) {
                                                 auto &callback = *static_cast<Callback *>(value);
                                                 callback.representation->add_item(id, callback.source_time);
                                             });
        source_runtime_type_->for_each_modified(source_time, source_->target_value, &callback,
                                                [](void *value, TimeSeriesItemId id) {
                                                    auto &callback = *static_cast<Callback *>(value);
                                                    callback.representation->update_item(*callback.context, id);
                                                });
        source_runtime_type_->for_each_removed(source_time, source_->target_value, &callback,
                                               [](void *value, TimeSeriesItemId id) {
                                                   auto &callback = *static_cast<Callback *>(value);
                                                   callback.representation->remove_item(*callback.context, id);
                                               });

        if (!source_runtime_type_->item_updated(source_time, source_->target_value, key_set_item_id)) {
            return;
        }

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = context.evaluation_time();
        context.notify(reference_.subscribers, was_valid ? OutputChange::update : OutputChange::revalidation);
    }

    void destroy(std::pmr::memory_resource &memory) noexcept override {
        for (const auto id : positions_) {
            DynamicSubscribers::release(items_[id].reference.subscribers);
        }
        items_.release(positions_, memory);
        positions_.release(memory);
        std::destroy_at(this);
        memory.deallocate(this, sizeof(TimeSeriesDictionaryReferenceRepresentation),
                          alignof(TimeSeriesDictionaryReferenceRepresentation));
    }

    [[nodiscard]] const ItemReference &item(TimeSeriesItemId id) const noexcept { return items_[id]; }

  private:
    void add_item(TimeSeriesItemId id, EvaluationTime reference_time) {
        if (positions_.contains(id)) {
            return;
        }

        auto &memory = source_->provider->memory();
        const auto target = source_->target_runtime_type->item(source_->target_value, id);
        items_.emplace_at(id, memory, *source_->provider, target, reference_time);
        positions_.set(id, memory);
    }

    void update_item(OutputNotificationContext &context, TimeSeriesItemId id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        const auto target = source_->target_runtime_type->item(source_->target_value, id);
        const auto now_valid = target.last_updated > MIN_DT;
        const auto change = now_valid ? (item.target_was_valid ? OutputChange::update : OutputChange::revalidation)
                                      : OutputChange::invalidation;
        item.reference.target_last_updated = &target.last_updated;
        item.reference.target_value = target.value;
        item.reference.target_runtime_type = &target.runtime_type;
        item.target_was_valid = now_valid;
        context.notify(item.reference.subscribers, change);
    }

    void remove_item(OutputNotificationContext &context, TimeSeriesItemId id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        item.reference.target_last_updated = &MIN_DT;
        item.reference.target_value = nullptr;
        item.target_was_valid = false;
        context.notify(item.reference.subscribers, OutputChange::invalidation);
        retired_.push_back(id);
    }

    void release_retired() noexcept {
        auto &memory = source_->provider->memory();
        for (const auto id : retired_) {
            auto &item = items_[id];
            DynamicSubscribers::release(item.reference.subscribers);
            items_.erase_at(id, memory);
            positions_.reset(id);
        }
        retired_.clear();
    }

    ReferenceStorage *source_;
    const TimeSeriesDictionaryRuntimeType *source_runtime_type_;
    TimeSeriesDictionaryReferenceRuntimeType runtime_type_;
    EvaluationTime last_updated_;
    ReferenceStorage reference_;
    util::occupancy_index positions_;
    util::sparse_bunch<ItemReference> items_;
    std::pmr::vector<TimeSeriesItemId> retired_;
};

class DereferencedTimeSeriesDictionaryRepresentation;

struct DereferencedItem {
    DereferencedItem(DereferencedTimeSeriesDictionaryRepresentation &owner, TimeSeriesItemId id,
                     const TimeSeriesRuntimeType &runtime_type) noexcept
        : owner{&owner}, runtime_type{&runtime_type}, id{id} {}

    DereferencedTimeSeriesDictionaryRepresentation *owner;
    ReferenceStorage *target{};
    const TimeSeriesRuntimeType *runtime_type;
    const void *value{};
    EvaluationTime last_updated{MIN_DT};
    TimeSeriesItemId id;
};

class DereferencedTimeSeriesDictionaryRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    DereferencedTimeSeriesDictionaryRuntimeType(const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &source,
                                                const TimeSeriesRuntimeType &item,
                                                DereferencedTimeSeriesDictionaryRepresentation &representation)
        : TimeSeriesDictionaryRuntimeType{type, source.key_runtime_type(), item}, representation_{&representation} {}

    [[nodiscard]] ConstItem item(const void *value, TimeSeriesItemId id) const override;
    [[nodiscard]] MutableItem item(void *, TimeSeriesItemId) const override;
    [[nodiscard]] bool item_updated(const EvaluationTime &last_updated, const void *value,
                                    TimeSeriesItemId id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified(const EvaluationTime &last_updated, const void *value, void *context,
                           ItemIdFunction function) const override;
    [[nodiscard]] bool item_modified(const EvaluationTime &last_updated, const void *value,
                                     TimeSeriesItemId id) const noexcept override;
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

  private:
    DereferencedTimeSeriesDictionaryRepresentation *representation_;
};

class DereferencedTimeSeriesDictionaryRepresentation final : public ReferenceRepresentation {
  public:
    DereferencedTimeSeriesDictionaryRepresentation(ReferenceProvider &provider, ReferenceStorage &source,
                                                   const TimeSeriesDictionaryType &type,
                                                   const TimeSeriesDictionaryRuntimeType &source_runtime_type,
                                                   const TimeSeriesRuntimeType &item_runtime_type)
        : provider_{&provider}, source_{&source}, source_runtime_type_{&source_runtime_type},
          runtime_type_{type, source_runtime_type, item_runtime_type, *this},
          last_updated_{*source.target_last_updated},
          reference_{&provider, &last_updated_, source.target_value, &runtime_type_, nullptr},
          retired_{&provider.memory()} {
        source_runtime_type.for_each_item_id(source.target_value, this, [](void *context, TimeSeriesItemId id) {
            auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(context);
            self.add_item(id, self.last_updated_);
        });
    }

    [[nodiscard]] const ReferenceStorage &source() const noexcept override { return *source_; }
    [[nodiscard]] ReferenceStorage &reference() noexcept override { return reference_; }

    void source_changed(OutputNotificationContext &context, OutputChange) override {
        release_retired();
        reset_changes(context.evaluation_time());

        const auto source_time = *source_->target_last_updated;
        struct AddCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime source_time;
        } add_callback{this, source_time};
        source_runtime_type_->for_each_added(source_time, source_->target_value, &add_callback,
                                             [](void *value, TimeSeriesItemId id) {
                                                 auto &callback = *static_cast<AddCallback *>(value);
                                                 callback.representation->add_item(id, callback.source_time);
                                             });
        source_runtime_type_->for_each_modified(*source_->target_last_updated, source_->target_value, this,
                                                [](void *value, TimeSeriesItemId id) {
                                                    auto &self = *static_cast<
                                                        DereferencedTimeSeriesDictionaryRepresentation *>(value);
                                                    self.bind_item(self.items_[id], self.current_source_item(id),
                                                                   *self.source_->target_last_updated);
                                                    self.modified_.set(id, self.provider_->memory());
                                                });
        source_runtime_type_->for_each_removed(*source_->target_last_updated, source_->target_value, this,
                                               [](void *value, TimeSeriesItemId id) {
                                                   static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value)
                                                       ->remove_item(id);
                                               });

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = *source_->target_last_updated;
        const auto now_valid = last_updated_ > MIN_DT;
        if (was_valid || now_valid) {
            context.notify(reference_.subscribers,
                           now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation)
                                     : OutputChange::invalidation);
        }
    }

    void destroy(std::pmr::memory_resource &memory) noexcept override {
        for (const auto id : positions_) {
            unsubscribe(items_[id]);
        }
        items_.release(positions_, memory);
        positions_.release(memory);
        modified_.release(memory);
        std::destroy_at(this);
        memory.deallocate(this, sizeof(DereferencedTimeSeriesDictionaryRepresentation),
                          alignof(DereferencedTimeSeriesDictionaryRepresentation));
    }

    [[nodiscard]] const DereferencedItem &item(TimeSeriesItemId id) const noexcept { return items_[id]; }
    [[nodiscard]] EvaluationTime change_time() const noexcept { return change_time_; }
    [[nodiscard]] const util::index<> &modified() const noexcept { return modified_; }

  private:
    [[nodiscard]] TimeSeriesRuntimeType::ConstItem current_source_item(TimeSeriesItemId id) const {
        return source_runtime_type_->item(source_->target_value, id);
    }

    void add_item(TimeSeriesItemId id, EvaluationTime sample_time) {
        if (positions_.contains(id)) {
            return;
        }

        auto &item = items_.emplace_at(id, provider_->memory(), *this, id, runtime_type_.item_runtime_type());
        positions_.set(id, provider_->memory());
        bind_item(item, current_source_item(id), sample_time);
    }

    void remove_item(TimeSeriesItemId id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        unsubscribe(item);
        item.value = nullptr;
        item.last_updated = MIN_DT;
        modified_.reset(id);
        retired_.push_back(id);
    }

    void bind_item(DereferencedItem &item, TimeSeriesRuntimeType::ConstItem source_item,
                   EvaluationTime sample_time) {
        unsubscribe(item);

        auto *target = source_item.last_updated > MIN_DT
                           ? *static_cast<ReferenceStorage *const *>(source_item.value)
                           : nullptr;
        if (target != nullptr) {
            target = &target->provider->representation(*target, runtime_type_.item_runtime_type().type());
        }

        item.target = target;
        if (target == nullptr || *target->target_last_updated <= MIN_DT) {
            item.runtime_type = &runtime_type_.item_runtime_type();
            item.value = nullptr;
            item.last_updated = MIN_DT;
            return;
        }

        item.runtime_type = target->target_runtime_type;
        item.value = target->target_value;
        item.last_updated = sample_time;
        auto &subscribers = DynamicSubscribers::ensure(target->subscribers, target->provider->memory());
        subscribers.add_observer(provider_->owner(), &item, &target_changed);
    }

    void unsubscribe(DereferencedItem &item) noexcept {
        if (item.target != nullptr && item.target->subscribers != nullptr) {
            item.target->subscribers->remove_observer(provider_->owner(), &item);
        }
        item.target = nullptr;
    }

    static void target_changed(OutputNotificationContext &context, void *target, OutputChange) {
        auto &item = *static_cast<DereferencedItem *>(target);
        item.owner->target_changed(context, item);
    }

    void target_changed(OutputNotificationContext &context, DereferencedItem &item) {
        reset_changes(context.evaluation_time());
        const auto now_valid = item.target != nullptr && *item.target->target_last_updated > MIN_DT;
        item.runtime_type = now_valid ? item.target->target_runtime_type : &runtime_type_.item_runtime_type();
        item.value = now_valid ? item.target->target_value : nullptr;
        item.last_updated = now_valid ? context.evaluation_time() : MIN_DT;
        modified_.set(item.id, provider_->memory());

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = context.evaluation_time();
        context.notify(reference_.subscribers, was_valid ? OutputChange::update : OutputChange::revalidation);
    }

    void reset_changes(EvaluationTime evaluation_time) {
        if (change_time_ == evaluation_time) {
            return;
        }
        modified_.clear();
        change_time_ = evaluation_time;
    }

    void release_retired() noexcept {
        auto &memory = provider_->memory();
        for (const auto id : retired_) {
            items_.erase_at(id, memory);
            positions_.reset(id);
        }
        retired_.clear();
    }

    ReferenceProvider *provider_;
    ReferenceStorage *source_;
    const TimeSeriesDictionaryRuntimeType *source_runtime_type_;
    DereferencedTimeSeriesDictionaryRuntimeType runtime_type_;
    EvaluationTime last_updated_;
    ReferenceStorage reference_;
    util::occupancy_index positions_;
    util::sparse_bunch<DereferencedItem> items_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
    std::pmr::vector<TimeSeriesItemId> retired_;
};

TimeSeriesRuntimeType::ConstItem TimeSeriesDictionaryReferenceRuntimeType::item(const void *value,
                                                                                TimeSeriesItemId id) const {
    if (id == key_set_item_id) {
        return TimeSeriesDictionaryRuntimeType::item(value, id);
    }

    const auto &item = representation_->item(id);
    return {item_runtime_type(), item.last_updated, &item.value};
}

TimeSeriesRuntimeType::MutableItem TimeSeriesDictionaryReferenceRuntimeType::item(void *, TimeSeriesItemId) const {
    throw std::logic_error{"a source-backed dictionary representation is immutable"};
}

bool TimeSeriesDictionaryReferenceRuntimeType::item_updated(const EvaluationTime &last_updated, const void *value,
                                                            TimeSeriesItemId id) const noexcept {
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

void TimeSeriesDictionaryReferenceRuntimeType::for_each_modified_item(
    const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time, void *context,
    ModifiedItemFunction function) const {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    const auto &keys = storage.key_storage().value;
    if (last_updated <= MIN_DT || storage.change_time() != evaluation_time) {
        return;
    }

    if (storage.key_storage().last_updated == evaluation_time) {
        const auto key_set = TimeSeriesDictionaryRuntimeType::item(value, key_set_item_id);
        function(context,
                 {key_set_item_id, &key_set.runtime_type, &storage.key_storage().last_updated,
                  &storage.key_storage().value});
    }
    for (const auto id : keys.added()) {
        const auto &item = representation_->item(id);
        function(context, {id, &item_runtime_type(), &item.last_updated, &item.value});
    }
    for (const auto id : keys.removed()) {
        const auto &item = representation_->item(id);
        function(context, {id, &item_runtime_type(), &MIN_DT, &item.value});
    }
}

TimeSeriesRuntimeType::ConstItem DereferencedTimeSeriesDictionaryRuntimeType::item(const void *value,
                                                                                   TimeSeriesItemId id) const {
    if (id == key_set_item_id) {
        return TimeSeriesDictionaryRuntimeType::item(value, id);
    }

    const auto &item = representation_->item(id);
    return {*item.runtime_type, item.last_updated, item.value};
}

TimeSeriesRuntimeType::MutableItem DereferencedTimeSeriesDictionaryRuntimeType::item(void *,
                                                                                     TimeSeriesItemId) const {
    throw std::logic_error{"a dereferenced dictionary representation is immutable"};
}

bool DereferencedTimeSeriesDictionaryRuntimeType::item_updated(const EvaluationTime &last_updated,
                                                               const void *value, TimeSeriesItemId id) const noexcept {
    if (TimeSeriesDictionaryRuntimeType::item_updated(last_updated, value, id)) {
        return true;
    }
    return representation_->change_time() == last_updated && representation_->modified().contains(id);
}

void DereferencedTimeSeriesDictionaryRuntimeType::for_each_updated_item(const EvaluationTime &last_updated,
                                                                        const void *value, void *context,
                                                                        ItemIdFunction function) const {
    TimeSeriesDictionaryRuntimeType::for_each_updated_item(last_updated, value, context, function);
    if (representation_->change_time() != last_updated) {
        return;
    }

    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    for (const auto id : representation_->modified()) {
        if (!storage.key_storage().value.added().contains(id)) {
            function(context, id);
        }
    }
}

void DereferencedTimeSeriesDictionaryRuntimeType::for_each_modified(const EvaluationTime &last_updated,
                                                                    const void *value, void *context,
                                                                    ItemIdFunction function) const {
    if (representation_->change_time() != last_updated) {
        return;
    }

    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    for (const auto id : representation_->modified()) {
        if (!storage.key_storage().value.added().contains(id)) {
            function(context, id);
        }
    }
}

bool DereferencedTimeSeriesDictionaryRuntimeType::item_modified(const EvaluationTime &last_updated,
                                                                const void *value, TimeSeriesItemId id) const noexcept {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    return representation_->change_time() == last_updated && representation_->modified().contains(id) &&
           !storage.key_storage().value.added().contains(id);
}

void DereferencedTimeSeriesDictionaryRuntimeType::for_each_modified_item(
    const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time, void *context,
    ModifiedItemFunction function) const {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    const auto &keys = storage.key_storage().value;
    if (last_updated <= MIN_DT) {
        function(context, {key_set_item_id, &TimeSeriesDictionaryRuntimeType::item(value, key_set_item_id).runtime_type,
                           &MIN_DT, &storage.key_storage().value});
        for (const auto id : keys.removed()) {
            const auto &item = representation_->item(id);
            function(context, {id, item.runtime_type, &MIN_DT, item.value});
        }
        return;
    }
    if (last_updated != evaluation_time) {
        return;
    }

    if (storage.key_storage().last_updated == evaluation_time) {
        const auto key_set = TimeSeriesDictionaryRuntimeType::item(value, key_set_item_id);
        function(context, {key_set_item_id, &key_set.runtime_type, &key_set.last_updated, key_set.value});
    }
    if (storage.change_time() == evaluation_time) {
        for (const auto id : keys.added()) {
            const auto &item = representation_->item(id);
            function(context, {id, item.runtime_type, &item.last_updated, item.value});
        }
        for (const auto id : keys.removed()) {
            const auto &item = representation_->item(id);
            function(context, {id, item.runtime_type, &MIN_DT, item.value});
        }
    }
    if (representation_->change_time() == evaluation_time) {
        for (const auto id : representation_->modified()) {
            if (!keys.added().contains(id)) {
                const auto &item = representation_->item(id);
                function(context, {id, item.runtime_type, &item.last_updated, item.value});
            }
        }
    }
}

} // namespace

ReferenceRepresentation *TimeSeriesDictionaryRuntimeType::create_reference_representation(
    ReferenceProvider &provider, ReferenceStorage &source, const TimeSeriesType &requested_type,
    std::pmr::memory_resource &memory) const {
    const auto *dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&requested_type);
    if (dictionary == nullptr || &dictionary->key_type() != &key_runtime_type().type()) {
        return TimeSeriesRuntimeType::create_reference_representation(provider, source, requested_type, memory);
    }

    const auto *requested_reference = dynamic_cast<const TimeSeriesReferenceType *>(&dictionary->value_type());
    if (requested_reference != nullptr && &requested_reference->target_type() == &item_runtime_type().type()) {
        const auto available = std::array{&item_runtime_type()};
        const auto &item = resolve_time_series_runtime_type(dictionary->value_type(), available);
        auto *storage = memory.allocate(sizeof(TimeSeriesDictionaryReferenceRepresentation),
                                        alignof(TimeSeriesDictionaryReferenceRepresentation));
        auto release = util::scope_exit{[&]() noexcept {
            memory.deallocate(storage, sizeof(TimeSeriesDictionaryReferenceRepresentation),
                              alignof(TimeSeriesDictionaryReferenceRepresentation));
        }};
        auto *result = std::construct_at(static_cast<TimeSeriesDictionaryReferenceRepresentation *>(storage),
                                         provider, source, *dictionary, *this, item);
        release.release();
        return result;
    }

    const auto *source_reference = dynamic_cast<const TimeSeriesReferenceType *>(&item_runtime_type().type());
    if (source_reference != nullptr && &source_reference->target_type() == &dictionary->value_type()) {
        const auto &reference_runtime_type = static_cast<const TimeSeriesReferenceRuntimeType &>(item_runtime_type());
        const auto available = std::array{&reference_runtime_type.target_runtime_type()};
        const auto &item = resolve_time_series_runtime_type(dictionary->value_type(), available);
        auto *storage = memory.allocate(sizeof(DereferencedTimeSeriesDictionaryRepresentation),
                                        alignof(DereferencedTimeSeriesDictionaryRepresentation));
        auto release = util::scope_exit{[&]() noexcept {
            memory.deallocate(storage, sizeof(DereferencedTimeSeriesDictionaryRepresentation),
                              alignof(DereferencedTimeSeriesDictionaryRepresentation));
        }};
        auto *result = std::construct_at(static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(storage),
                                         provider, source, *dictionary, *this, item);
        release.release();
        return result;
    }

    return TimeSeriesRuntimeType::create_reference_representation(provider, source, requested_type, memory);
}

} // namespace bg::details
