#include <bg/time_series/tsd/tsd_runtime_type.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace bg::details {
namespace {

class TimeSeriesDictionaryReferenceRepresentation;

class RoutedReferenceRuntimeType final : public TimeSeriesReferenceRuntimeType {
  public:
    explicit RoutedReferenceRuntimeType(const TimeSeriesReferenceType &type) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(ReferenceRoute), alignof(ReferenceRoute)} {}

    ReferenceAddress reference_address(const void *value) const override {
        return ReferenceAddress{const_cast<ReferenceRoute *>(static_cast<const ReferenceRoute *>(value))};
    }
};

class TimeSeriesDictionaryReferenceRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    TimeSeriesDictionaryReferenceRuntimeType(const TimeSeriesDictionaryType &type,
                                             const TimeSeriesDictionaryRuntimeType &input,
                                             const TimeSeriesRuntimeType &item,
                                             TimeSeriesDictionaryReferenceRepresentation &representation)
        : TimeSeriesDictionaryRuntimeType{type, input.key_runtime_type(), item}, representation_{&representation} {}

    ConstItem item(const void *value, TimeSeriesItemId id) const override;
    MutableItem item(void *, TimeSeriesItemId) const override;
    bool item_updated(const EvaluationTime &last_updated, const void *value,
                      TimeSeriesItemId id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified(const EvaluationTime &, const void *, void *, ItemIdFunction) const override {}
    bool item_modified(const EvaluationTime &, const void *, TimeSeriesItemId) const noexcept override { return false; }
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

  private:
    TimeSeriesDictionaryReferenceRepresentation *representation_;
};

struct ItemReference {
    ItemReference(ReferenceAddress parent, TimeSeriesItemId id, EvaluationTime reference_time)
        : last_updated{reference_time} {
        reference.bind_item(parent, id);
    }

    ReferenceRoute reference;
    EvaluationTime last_updated;
};

static_assert(sizeof(ItemReference) == 4 * sizeof(void *));

struct RetiredItem {
    TimeSeriesItemId id;
    EvaluationTime time;
};

class TimeSeriesDictionaryReferenceRepresentation final : public ReferenceRepresentation {
  public:
    TimeSeriesDictionaryReferenceRepresentation(ReferenceProvider &provider, ReferenceAddress input,
                                                const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &input_runtime_type,
                                                const TimeSeriesRuntimeType &item_runtime_type)
        : ReferenceRepresentation{input}, provider_{&provider}, input_runtime_type_{&input_runtime_type},
          item_runtime_type_{static_cast<const TimeSeriesReferenceType &>(item_runtime_type.type())},
          runtime_type_{type, input_runtime_type, item_runtime_type_, *this},
          last_updated_{*reference_target(input).last_updated}, retired_{&provider.memory()} {
        const auto input_target = reference_target(input);
        const auto *storage = input_target.value;
        input_runtime_type.for_each_item_id(storage, this, [](void *context, TimeSeriesItemId id) {
            auto &self = *static_cast<TimeSeriesDictionaryReferenceRepresentation *>(context);
            self.add_item(id, self.last_updated_);
        });
    }

    ReferenceTarget target() const override {
        return {&last_updated_, reference_target(input()).value, &runtime_type_};
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

        const auto input_target = reference_target(input());
        const auto input_time = *input_target.last_updated;
        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputChangeContext *context;
            EvaluationTime input_time;
        } callback{this, &context, input_time};

        input_runtime_type_->for_each_added(input_time, input_target.value, &callback,
                                            [](void *value, TimeSeriesItemId id) {
                                                auto &callback = *static_cast<Callback *>(value);
                                                callback.representation->add_item(id, callback.input_time);
                                            });
        input_runtime_type_->for_each_modified(input_time, input_target.value, &callback,
                                               [](void *value, TimeSeriesItemId id) {
                                                   auto &callback = *static_cast<Callback *>(value);
                                                   callback.representation->update_item(*callback.context, id);
                                               });
        input_runtime_type_->for_each_removed(input_time, input_target.value, &callback,
                                              [](void *value, TimeSeriesItemId id) {
                                                  auto &callback = *static_cast<Callback *>(value);
                                                  callback.representation->remove_item(*callback.context, id);
                                              });

        if (!input_runtime_type_->item_updated(input_time, input_target.value, key_set_item_id)) {
            provider_->publish_item_notification_batch(context);
            cancel_items.release();
            return;
        }

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = context.evaluation_time();
        notify_reference(address(), context, was_valid ? OutputChange::update : OutputChange::revalidation);
        provider_->publish_item_notification_batch(context);
        cancel_items.release();
    }

    void destroy(std::pmr::memory_resource &memory) noexcept override {
        items_.release(positions_, memory);
        positions_.release(memory);
        target_valid_.release(memory);
        std::destroy_at(this);
        memory.deallocate(this, sizeof(TimeSeriesDictionaryReferenceRepresentation),
                          alignof(TimeSeriesDictionaryReferenceRepresentation));
    }

    const ItemReference &item(TimeSeriesItemId id) const noexcept { return items_[id]; }

  private:
    void invalidate(OutputChangeContext &context) {
        if (last_updated_ <= MIN_DT) {
            return;
        }

        for (const auto id : target_valid_) {
            provider_->notify(items_[id].reference, context, OutputChange::invalidation);
        }
        target_valid_.clear();
        last_updated_ = MIN_DT;
        notify_reference(address(), context, OutputChange::invalidation);
    }

    void revalidate(OutputChangeContext &context) {
        auto current = util::index<>{};
        auto release_current = util::scope_exit{[&]() noexcept { current.release(provider_->memory()); }};
        const auto input_target = reference_target(input());

        struct Callback {
            TimeSeriesDictionaryReferenceRepresentation *representation;
            OutputChangeContext *context;
            util::index<> *current;
        } callback{this, &context, &current};
        input_runtime_type_->for_each_item_id(input_target.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            callback.current->set(id, self.provider_->memory());
            if (!self.positions_.contains(id)) {
                self.add_item(id, callback.context->evaluation_time());
                return;
            }

            self.restore_item(id);
            const auto target = self.input_runtime_type_->item(reference_target(self.input()).value, id);
            if (target.last_updated <= MIN_DT) {
                self.target_valid_.reset(id);
                return;
            }

            self.target_valid_.set(id, self.provider_->memory());
            self.provider_->notify(self.items_[id].reference, *callback.context, OutputChange::revalidation);
        });

        for (const auto id : positions_) {
            if (!current.contains(id)) {
                retire_item(id, context.evaluation_time());
            }
        }

        last_updated_ = context.evaluation_time();
        notify_reference(address(), context, OutputChange::revalidation);
    }

    void add_item(TimeSeriesItemId id, EvaluationTime reference_time) {
        if (positions_.contains(id)) {
            restore_item(id);
            return;
        }

        auto &memory = provider_->memory();
        const auto input_target = reference_target(input());
        const auto target = input_runtime_type_->item(input_target.value, id);
        items_.emplace_at(id, memory, input(), id, reference_time);
        positions_.set(id, memory);
        if (target.last_updated > MIN_DT) {
            target_valid_.set(id, memory);
        }
    }

    void update_item(OutputChangeContext &context, TimeSeriesItemId id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        const auto input_target = reference_target(input());
        const auto target = input_runtime_type_->item(input_target.value, id);
        const auto now_valid = target.last_updated > MIN_DT;
        const auto was_valid = target_valid_.contains(id);
        if (now_valid) {
            target_valid_.set(id, provider_->memory());
            provider_->notify(item.reference, context,
                              was_valid ? OutputChange::update : OutputChange::revalidation);
            return;
        }
        if (was_valid) {
            target_valid_.reset(id);
            provider_->notify(item.reference, context, OutputChange::invalidation);
        }
    }

    void remove_item(OutputChangeContext &context, TimeSeriesItemId id) {
        if (!positions_.contains(id)) {
            return;
        }

        auto &item = items_[id];
        if (target_valid_.reset(id)) {
            provider_->notify(item.reference, context, OutputChange::invalidation);
        }
        retire_item(id, context.evaluation_time());
    }

    void retire_item(TimeSeriesItemId id, EvaluationTime time) {
        const auto existing = std::ranges::find(retired_, id, &RetiredItem::id);
        if (existing == retired_.end()) {
            retired_.push_back({id, time});
        }
    }

    void restore_item(TimeSeriesItemId id) {
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
            if (!provider_->release_route(items_[item.id].reference)) {
                *retained++ = item;
                continue;
            }
            items_.erase_at(item.id, memory);
            positions_.reset(item.id);
        }
        retired_.erase(retained, retired_.end());
    }

    ReferenceProvider *provider_;
    const TimeSeriesDictionaryRuntimeType *input_runtime_type_;
    RoutedReferenceRuntimeType item_runtime_type_;
    TimeSeriesDictionaryReferenceRuntimeType runtime_type_;
    EvaluationTime last_updated_;
    util::occupancy_index positions_;
    util::index<> target_valid_;
    util::sparse_bunch<ItemReference> items_;
    std::pmr::vector<RetiredItem> retired_;
};

class DereferencedTimeSeriesDictionaryRepresentation;
struct SampledDictionary;

class SampledDictionaryRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    explicit SampledDictionaryRuntimeType(const TimeSeriesDictionaryRuntimeType &shape)
        : TimeSeriesDictionaryRuntimeType{static_cast<const TimeSeriesDictionaryType &>(shape.type()),
                                          shape.key_runtime_type(), shape.item_runtime_type()} {}

    DictionaryKeyStorage retention_storage(void *value) const noexcept override;
    RetainedDictionaryKey retain_key(void *, const ScalarData &, std::pmr::memory_resource &) const override;
    void release_key(void *, TimeSeriesItemId, std::pmr::memory_resource &) const noexcept override;
    TimeSeriesItemId find_key(const void *value, const ScalarData &key) const override;
    bool key_live(const void *value, TimeSeriesItemId id) const noexcept override;
    TimeSeriesItemId find_item(const void *value, const ScalarData &key) const override;
    const void *key(const void *value, TimeSeriesItemId id) const noexcept override;
    void for_each_added(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    void for_each_removed(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    void for_each_modified(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    bool item_modified(const EvaluationTime &, const void *, TimeSeriesItemId id) const noexcept override;
    std::size_t item_count(const void *value) const override;
    ConstItem item(const void *value, TimeSeriesItemId id) const override;
    MutableItem item(void *, TimeSeriesItemId) const override;
    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override;
    bool item_updated(const EvaluationTime &, const void *, TimeSeriesItemId id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    bool collection_valid(const void *value) const noexcept override;
    void for_each_modified_item(const EvaluationTime &, const void *, EvaluationTime, void *,
                                ModifiedItemFunction) const override;

};

const SampledDictionaryRuntimeType &sampled_runtime_type(const TimeSeriesDictionaryRuntimeType &shape);

struct SampledDictionary {
    struct Child {
        TimeSeriesItemId id;
        SampledDictionary *sample;
    };

    SampledDictionary(const TimeSeriesDictionaryRuntimeType &shape, ReferenceTarget previous,
                      ReferenceTarget current, EvaluationTime evaluation_time, std::pmr::memory_resource &memory)
        : runtime_type{&sampled_runtime_type(shape)},
          previous_last_updated{previous.last_updated == nullptr ? MIN_DT : *previous.last_updated},
          current_last_updated{current.last_updated == nullptr ? MIN_DT : *current.last_updated}, previous{previous},
          current{current}, evaluation_time{evaluation_time}, children{&memory}, memory{&memory} {
        this->previous.last_updated = &previous_last_updated;
        this->current.last_updated = &current_last_updated;
    }

    const SampledDictionaryRuntimeType *runtime_type;
    EvaluationTime previous_last_updated;
    EvaluationTime current_last_updated;
    ReferenceTarget previous;
    ReferenceTarget current;
    EvaluationTime evaluation_time;
    util::index<> added;
    util::index<> removed;
    util::index<> modified;
    std::pmr::vector<Child> children;
    std::pmr::memory_resource *memory;
};

SampledDictionary *make_dictionary_sample(ReferenceTarget previous, ReferenceTarget current,
                                          EvaluationTime evaluation_time, std::pmr::memory_resource &memory);
void destroy_dictionary_sample(SampledDictionary *sample) noexcept;
bool valid_target(ReferenceTarget target) noexcept;

class DereferencedTimeSeriesDictionaryRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    DereferencedTimeSeriesDictionaryRuntimeType(const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &input,
                                                const TimeSeriesRuntimeType &item,
                                                DereferencedTimeSeriesDictionaryRepresentation &representation)
        : TimeSeriesDictionaryRuntimeType{type, input.key_runtime_type(), item}, representation_{&representation} {}

    ConstItem item(const void *value, TimeSeriesItemId id) const override;
    MutableItem item(void *, TimeSeriesItemId) const override;
    bool item_updated(const EvaluationTime &last_updated, const void *value,
                      TimeSeriesItemId id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified(const EvaluationTime &last_updated, const void *value, void *context,
                           ItemIdFunction function) const override;
    bool item_modified(const EvaluationTime &last_updated, const void *value,
                       TimeSeriesItemId id) const noexcept override;
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

  private:
    DereferencedTimeSeriesDictionaryRepresentation *representation_;
};

class DereferencedTimeSeriesDictionaryRepresentation final : public ReferenceRepresentation {
  public:
    DereferencedTimeSeriesDictionaryRepresentation(ReferenceProvider &provider, ReferenceAddress input,
                                                   const TimeSeriesDictionaryType &type,
                                                   const TimeSeriesDictionaryRuntimeType &input_runtime_type,
                                                   const TimeSeriesRuntimeType &item_runtime_type)
        : ReferenceRepresentation{input}, provider_{&provider}, input_runtime_type_{&input_runtime_type},
          runtime_type_{type, input_runtime_type, item_runtime_type, *this},
          last_updated_{*reference_target(input).last_updated}, targets_{&provider.memory()} {
        const auto input_target = reference_target(input);
        input_runtime_type.for_each_item_id(input_target.value, this, [](void *context, TimeSeriesItemId id) {
            auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(context);
            self.add_item(id, self.last_updated_);
        });
    }

    ReferenceTarget target() const override {
        return {&last_updated_, reference_target(input()).value, &runtime_type_};
    }

    void notify(OutputChangeContext &context, OutputChange change) override {
        reset_changes(context.evaluation_time());

        if (change == OutputChange::invalidation) {
            invalidate(context);
            return;
        }
        if (last_updated_ <= MIN_DT) {
            revalidate(context);
            return;
        }

        const auto input_target = reference_target(input());
        const auto input_time = *input_target.last_updated;
        struct AddCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime input_time;
        } add_callback{this, input_time};
        input_runtime_type_->for_each_added(input_time, input_target.value, &add_callback,
                                            [](void *value, TimeSeriesItemId id) {
                                                auto &callback = *static_cast<AddCallback *>(value);
                                                callback.representation->add_item(id, callback.input_time);
                                            });
        input_runtime_type_->for_each_modified(
            input_time, input_target.value, this, [](void *value, TimeSeriesItemId id) {
                auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value);
                self.bind_item(id, self.current_input_item(id), *reference_target(self.input()).last_updated);
                self.modified_.set(id, self.provider_->memory());
            });
        struct RemoveCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime evaluation_time;
        } remove_callback{this, context.evaluation_time()};
        input_runtime_type_->for_each_removed(input_time, input_target.value, &remove_callback,
                                              [](void *value, TimeSeriesItemId id) {
                                                  auto &callback = *static_cast<RemoveCallback *>(value);
                                                  callback.representation->remove_item(id, callback.evaluation_time);
                                              });

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = input_time;
        const auto now_valid = last_updated_ > MIN_DT;
        if (was_valid || now_valid) {
            notify_reference(address(), context,
                             now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation)
                                       : OutputChange::invalidation);
        }
    }

    void destroy(std::pmr::memory_resource &memory) noexcept override {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
        }
        for (auto id = TimeSeriesItemId{}; id < targets_.size(); ++id) {
            unsubscribe(id);
        }
        clear_samples();
        modified_.release(memory);
        std::destroy_at(this);
        memory.deallocate(this, sizeof(DereferencedTimeSeriesDictionaryRepresentation),
                          alignof(DereferencedTimeSeriesDictionaryRepresentation));
    }

    TimeSeriesRuntimeType::ConstItem item(TimeSeriesItemId id) const {
        if (const auto *sample = samples_.get(id, sample_positions_)) {
            const auto &visible_time = valid_target((*sample)->current) ? (*sample)->evaluation_time : MIN_DT;
            return {*(*sample)->runtime_type, visible_time, *sample};
        }
        if (id >= targets_.size() || !targets_[id]) {
            return {runtime_type_.item_runtime_type(), MIN_DT, nullptr};
        }

        const auto target = reference_target(targets_[id]);
        if (!valid_target(target)) {
            return {runtime_type_.item_runtime_type(), MIN_DT, nullptr};
        }

        const auto input_item = current_input_item(id);
        const auto &visible_time = input_item.last_updated > *target.last_updated
                                       ? input_item.last_updated
                                       : *target.last_updated;
        return {*target.runtime_type, visible_time, target.value};
    }
    EvaluationTime change_time() const noexcept { return change_time_; }
    const util::index<> &modified() const noexcept { return modified_; }

  private:
    void invalidate(OutputChangeContext &context) {
        if (last_updated_ <= MIN_DT) {
            return;
        }

        for (auto id = TimeSeriesItemId{}; id < targets_.size(); ++id) {
            if (!targets_[id]) {
                continue;
            }
            sample_switch(id, {}, context.evaluation_time());
            unsubscribe(id);
        }
        last_updated_ = MIN_DT;
        notify_reference(address(), context, OutputChange::invalidation);
    }

    void revalidate(OutputChangeContext &context) {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
            clear_samples();
        }
        for (auto id = TimeSeriesItemId{}; id < targets_.size(); ++id) {
            unsubscribe(id);
        }
        targets_.clear();
        modified_.clear();

        const auto input_target = reference_target(input());
        struct Callback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime evaluation_time;
        } callback{this, context.evaluation_time()};
        input_runtime_type_->for_each_item_id(input_target.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            self.add_item(id, callback.evaluation_time);
            self.modified_.set(id, self.provider_->memory());
        });

        last_updated_ = context.evaluation_time();
        notify_reference(address(), context, OutputChange::revalidation);
    }

    TimeSeriesRuntimeType::ConstItem current_input_item(TimeSeriesItemId id) const {
        return input_runtime_type_->item(reference_target(input()).value, id);
    }

    void add_item(TimeSeriesItemId id, EvaluationTime sample_time) {
        if (targets_.size() <= id) {
            targets_.resize(static_cast<std::size_t>(id) + 1);
        }
        bind_item(id, current_input_item(id), sample_time);
    }

    void remove_item(TimeSeriesItemId id, EvaluationTime evaluation_time) {
        if (id >= targets_.size() || !targets_[id]) {
            return;
        }

        sample_switch(id, {}, evaluation_time);
        unsubscribe(id);
        modified_.reset(id);
    }

    void bind_item(TimeSeriesItemId id, TimeSeriesRuntimeType::ConstItem input_item, EvaluationTime sample_time) {
        const auto previous_address = targets_[id];
        const auto previous = previous_address ? reference_target(previous_address) : ReferenceTarget{};

        auto target = input_item.last_updated > MIN_DT ? input_item.runtime_type.reference_address(input_item.value)
                                                       : ReferenceAddress{};
        if (target) {
            target = reference_representation(target, runtime_type_.item_runtime_type().type());
        }
        if (target == previous_address) {
            return;
        }

        unsubscribe(id);
        const auto target_value = target ? reference_target(target) : ReferenceTarget{};
        sample_switch(id, target_value, sample_time, previous);
        targets_[id] = target;
        if (target) {
            add_reference_item_notification_link(target, provider_->owner(), this, id, &target_changed_callback,
                                                 &publish_target_changes_callback);
        }
    }

    void sample_switch(TimeSeriesItemId id, ReferenceTarget current, EvaluationTime evaluation_time,
                       ReferenceTarget previous = {}) {
        auto previous_last_updated = MIN_DT;
        if (auto *existing = samples_.get(id, sample_positions_)) {
            previous = (*existing)->previous;
            previous_last_updated = *previous.last_updated;
            previous.last_updated = &previous_last_updated;
            destroy_dictionary_sample(*existing);
            samples_.erase_at(id, provider_->memory());
            sample_positions_.reset(id);
        }

        if (previous.runtime_type == nullptr && id < targets_.size() && targets_[id]) {
            previous = reference_target(targets_[id]);
        }
        auto *sample = make_dictionary_sample(previous, current, evaluation_time, provider_->memory());
        if (sample == nullptr) {
            return;
        }
        auto destroy_sample = util::scope_exit{[&]() noexcept { destroy_dictionary_sample(sample); }};
        if (!sample_cleanup_registered_) {
            provider_->add_cycle_callback(this, clear_samples_callback);
            sample_cleanup_registered_ = true;
        }

        sample_positions_.set(id, provider_->memory());
        auto reset_position = util::scope_exit{[&]() noexcept { sample_positions_.reset(id); }};
        samples_.emplace_at(id, provider_->memory(), sample);
        reset_position.release();
        destroy_sample.release();
    }

    static void clear_samples_callback(void *value) noexcept {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value)->clear_samples();
    }

    void clear_samples() noexcept {
        auto &memory = provider_->memory();
        for (const auto id : sample_positions_) {
            destroy_dictionary_sample(samples_[id]);
        }
        samples_.release(sample_positions_, memory);
        sample_positions_.release(memory);
        sample_cleanup_registered_ = false;
    }

    void unsubscribe(TimeSeriesItemId id) noexcept {
        if (id < targets_.size() && targets_[id]) {
            remove_reference_item_notification_link(targets_[id], provider_->owner(), this, id,
                                                    &target_changed_callback);
        }
        if (id < targets_.size()) {
            targets_[id] = nullptr;
        }
    }

    static void target_changed_callback(OutputChangeContext &context, void *target, TimeSeriesItemId id,
                                        OutputChange change) {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(target)->target_changed(context, id, change);
    }

    static void publish_target_changes_callback(OutputChangeContext &context, void *target) {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(target)->publish_target_changes(context);
    }

    void target_changed(OutputChangeContext &context, TimeSeriesItemId id, OutputChange change) {
        reset_changes(context.evaluation_time());
        const auto target = id < targets_.size() && targets_[id] ? reference_target(targets_[id]) : ReferenceTarget{};
        if (change == OutputChange::invalidation) {
            auto previous = target;
            previous.last_updated = &last_updated_;
            sample_switch(id, {}, context.evaluation_time(), previous);
        } else if (change == OutputChange::revalidation) {
            auto previous = target;
            previous.last_updated = &MIN_DT;
            previous.value = nullptr;
            sample_switch(id, target, context.evaluation_time(), previous);
        }
        modified_.set(id, provider_->memory());
    }

    void publish_target_changes(OutputChangeContext &context) {
        if (last_updated_ == context.evaluation_time()) {
            return;
        }

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = context.evaluation_time();
        notify_reference(address(), context, was_valid ? OutputChange::update : OutputChange::revalidation);
    }

    void reset_changes(EvaluationTime evaluation_time) {
        if (change_time_ == evaluation_time) {
            return;
        }
        modified_.clear();
        change_time_ = evaluation_time;
    }

    ReferenceProvider *provider_;
    const TimeSeriesDictionaryRuntimeType *input_runtime_type_;
    DereferencedTimeSeriesDictionaryRuntimeType runtime_type_;
    EvaluationTime last_updated_;
    std::pmr::vector<ReferenceAddress> targets_;
    util::occupancy_index sample_positions_;
    util::sparse_bunch<SampledDictionary *> samples_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
    bool sample_cleanup_registered_{};
};

constexpr auto previous_item_flag = TimeSeriesItemId{1} << 31;

bool valid_target(ReferenceTarget target) noexcept {
    return target.runtime_type != nullptr && target.value != nullptr && target.last_updated != nullptr &&
           *target.last_updated > MIN_DT;
}

const TimeSeriesDictionaryRuntimeType *dictionary_runtime_type(ReferenceTarget target) noexcept {
    return target.runtime_type == nullptr
               ? nullptr
               : dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
}

bool same_item(const TimeSeriesRuntimeType::ConstItem &left,
               const TimeSeriesRuntimeType::ConstItem &right) noexcept {
    if (dynamic_cast<const TimeSeriesReferenceType *>(&left.runtime_type.type()) != nullptr &&
        dynamic_cast<const TimeSeriesReferenceType *>(&right.runtime_type.type()) != nullptr &&
        left.value != nullptr && right.value != nullptr) {
        return left.runtime_type.reference_address(left.value) == right.runtime_type.reference_address(right.value);
    }
    return &left.runtime_type == &right.runtime_type && left.value == right.value;
}

const SampledDictionaryRuntimeType &sampled_runtime_type(const TimeSeriesDictionaryRuntimeType &shape) {
    struct Entry {
        const TimeSeriesDictionaryRuntimeType *shape;
        std::unique_ptr<SampledDictionaryRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find(entries, &shape, &Entry::shape);
    if (existing != entries.end()) {
        return *existing->runtime_type;
    }

    auto runtime_type = std::make_unique<SampledDictionaryRuntimeType>(shape);
    const auto &result = *runtime_type;
    entries.push_back({&shape, std::move(runtime_type)});
    return result;
}

SampledDictionary *make_dictionary_sample(ReferenceTarget previous, ReferenceTarget current,
                                          EvaluationTime evaluation_time, std::pmr::memory_resource &memory) {
    const auto *previous_type = dictionary_runtime_type(previous);
    const auto *current_type = dictionary_runtime_type(current);
    if (previous_type == nullptr && current_type == nullptr) {
        return nullptr;
    }
    if ((previous.runtime_type != nullptr && previous_type == nullptr) ||
        (current.runtime_type != nullptr && current_type == nullptr)) {
        return nullptr;
    }

    const auto &shape = current_type != nullptr ? *current_type : *previous_type;
    auto *storage = memory.allocate(sizeof(SampledDictionary), alignof(SampledDictionary));
    auto release = util::scope_exit{
        [&]() noexcept { memory.deallocate(storage, sizeof(SampledDictionary), alignof(SampledDictionary)); }};
    auto *sample = std::construct_at(static_cast<SampledDictionary *>(storage), shape, previous, current,
                                     evaluation_time, memory);
    auto destroy = util::scope_exit{[&]() noexcept { destroy_dictionary_sample(sample); }};

    if (valid_target(current)) {
        struct CurrentCallback {
            SampledDictionary *sample;
            const TimeSeriesDictionaryRuntimeType *current_type;
            const TimeSeriesDictionaryRuntimeType *previous_type;
        } callback{sample, current_type, previous_type};
        current_type->for_each_item_id(current.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<CurrentCallback *>(value);
            auto &sample = *callback.sample;
            const auto key = ScalarData{callback.current_type->key_runtime_type(),
                                        callback.current_type->key(sample.current.value, id)};
            const auto previous_id = valid_target(sample.previous)
                                         ? callback.previous_type->find_item(sample.previous.value, key)
                                         : util::occupancy_index::npos;
            if (previous_id == util::occupancy_index::npos) {
                sample.added.set(id, *sample.memory);
                return;
            }

            const auto current_item = callback.current_type->item(sample.current.value, id);
            const auto previous_item = callback.previous_type->item(sample.previous.value, previous_id);
            if (same_item(current_item, previous_item)) {
                if (callback.current_type->item_modified(*sample.current.last_updated, sample.current.value, id)) {
                    sample.modified.set(id, *sample.memory);
                }
                return;
            }

            sample.modified.set(id, *sample.memory);
            const auto previous_target = ReferenceTarget{&previous_item.last_updated, previous_item.value,
                                                         &previous_item.runtime_type};
            const auto current_target = ReferenceTarget{&current_item.last_updated, current_item.value,
                                                        &current_item.runtime_type};
            if (auto *child = make_dictionary_sample(previous_target, current_target, sample.evaluation_time,
                                                     *sample.memory)) {
                sample.children.push_back({id, child});
            }
        });
    }

    if (valid_target(previous)) {
        struct PreviousCallback {
            SampledDictionary *sample;
            const TimeSeriesDictionaryRuntimeType *current_type;
            const TimeSeriesDictionaryRuntimeType *previous_type;
        } callback{sample, current_type, previous_type};
        previous_type->for_each_item_id(previous.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<PreviousCallback *>(value);
            auto &sample = *callback.sample;
            const auto key = ScalarData{callback.previous_type->key_runtime_type(),
                                        callback.previous_type->key(sample.previous.value, id)};
            if (valid_target(sample.current) &&
                callback.current_type->find_item(sample.current.value, key) != util::occupancy_index::npos) {
                return;
            }
            sample.removed.set(id, *sample.memory);
        });
    }

    destroy.release();
    release.release();
    return sample;
}

void destroy_dictionary_sample(SampledDictionary *sample) noexcept {
    if (sample == nullptr) {
        return;
    }

    auto &memory = *sample->memory;
    for (const auto child : sample->children) {
        destroy_dictionary_sample(child.sample);
    }
    sample->added.release(memory);
    sample->removed.release(memory);
    sample->modified.release(memory);
    std::destroy_at(sample);
    memory.deallocate(sample, sizeof(SampledDictionary), alignof(SampledDictionary));
}

TimeSeriesItemId SampledDictionaryRuntimeType::find_item(const void *value, const ScalarData &key) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (!valid_target(sample.current)) {
        return util::occupancy_index::npos;
    }
    return dictionary_runtime_type(sample.current)->find_item(sample.current.value, key);
}

RetainedDictionaryKey SampledDictionaryRuntimeType::retain_key(void *value, const ScalarData &key,
                                                               std::pmr::memory_resource &memory) const {
    auto &sample = *static_cast<SampledDictionary *>(value);
    auto *runtime_type = dictionary_runtime_type(sample.current);
    if (runtime_type == nullptr || sample.current.value == nullptr) {
        return {};
    }

    return runtime_type->retain_key(const_cast<void *>(sample.current.value), key, memory);
}

DictionaryKeyStorage SampledDictionaryRuntimeType::retention_storage(void *value) const noexcept {
    auto &sample = *static_cast<SampledDictionary *>(value);
    auto *runtime_type = dictionary_runtime_type(sample.current);
    if (runtime_type == nullptr || sample.current.value == nullptr) {
        return {};
    }

    return runtime_type->retention_storage(const_cast<void *>(sample.current.value));
}

void SampledDictionaryRuntimeType::release_key(void *, TimeSeriesItemId,
                                               std::pmr::memory_resource &) const noexcept {
    assert(false && "sampled dictionary key retention must be released through its retained runtime type");
}

TimeSeriesItemId SampledDictionaryRuntimeType::find_key(const void *value, const ScalarData &key) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    const auto *runtime_type = dictionary_runtime_type(sample.current);
    if (runtime_type == nullptr || sample.current.value == nullptr) {
        return util::occupancy_index::npos;
    }

    return runtime_type->find_key(sample.current.value, key);
}

bool SampledDictionaryRuntimeType::key_live(const void *value, TimeSeriesItemId id) const noexcept {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    const auto *runtime_type = dictionary_runtime_type(sample.current);
    return runtime_type != nullptr && sample.current.value != nullptr &&
           runtime_type->key_live(sample.current.value, id);
}

const void *SampledDictionaryRuntimeType::key(const void *value, TimeSeriesItemId id) const noexcept {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if ((id & previous_item_flag) != 0) {
        return dictionary_runtime_type(sample.previous)->key(sample.previous.value, id & ~previous_item_flag);
    }
    return dictionary_runtime_type(sample.current)->key(sample.current.value, id);
}

void SampledDictionaryRuntimeType::for_each_added(const EvaluationTime &, const void *value, void *context,
                                                  ItemIdFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    for (const auto id : sample.added) {
        function(context, id);
    }
}

void SampledDictionaryRuntimeType::for_each_removed(const EvaluationTime &, const void *value, void *context,
                                                    ItemIdFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    for (const auto id : sample.removed) {
        function(context, id | previous_item_flag);
    }
}

void SampledDictionaryRuntimeType::for_each_modified(const EvaluationTime &, const void *value, void *context,
                                                     ItemIdFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    for (const auto id : sample.modified) {
        function(context, id);
    }
}

bool SampledDictionaryRuntimeType::item_modified(const EvaluationTime &, const void *value,
                                                 TimeSeriesItemId id) const noexcept {
    return static_cast<const SampledDictionary *>(value)->modified.contains(id);
}

std::size_t SampledDictionaryRuntimeType::item_count(const void *value) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    return valid_target(sample.current) ? dictionary_runtime_type(sample.current)->item_count(sample.current.value) : 0;
}

TimeSeriesRuntimeType::ConstItem SampledDictionaryRuntimeType::item(const void *value, TimeSeriesItemId id) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (id == key_set_item_id) {
        const auto target = valid_target(sample.current) ? sample.current : sample.previous;
        auto item = dictionary_runtime_type(target)->item(target.value, id);
        return {item.runtime_type, sample.evaluation_time, item.value};
    }
    if ((id & previous_item_flag) != 0) {
        const auto item =
            dictionary_runtime_type(sample.previous)->item(sample.previous.value, id & ~previous_item_flag);
        return {item.runtime_type, MIN_DT, item.value};
    }

    const auto child = std::ranges::lower_bound(sample.children, id, {}, &SampledDictionary::Child::id);
    if (child != sample.children.end() && child->id == id) {
        const auto &visible_time = valid_target(child->sample->current) ? child->sample->evaluation_time : MIN_DT;
        return {*child->sample->runtime_type, visible_time, child->sample};
    }

    const auto item = dictionary_runtime_type(sample.current)->item(sample.current.value, id);
    const auto &last_updated = sample.added.contains(id) || sample.modified.contains(id)
                                   ? sample.evaluation_time
                                   : item.last_updated;
    return {item.runtime_type, last_updated, item.value};
}

TimeSeriesRuntimeType::MutableItem SampledDictionaryRuntimeType::item(void *, TimeSeriesItemId) const {
    throw std::logic_error{"a sampled dictionary is immutable"};
}

void SampledDictionaryRuntimeType::for_each_item_id(const void *value, void *context, ItemIdFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (valid_target(sample.current)) {
        dictionary_runtime_type(sample.current)->for_each_item_id(sample.current.value, context, function);
    }
}

bool SampledDictionaryRuntimeType::item_updated(const EvaluationTime &, const void *value,
                                                TimeSeriesItemId id) const noexcept {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (id == key_set_item_id) {
        return sample.added.any() || sample.removed.any();
    }
    if ((id & previous_item_flag) != 0) {
        return sample.removed.contains(id & ~previous_item_flag);
    }
    return sample.added.contains(id) || sample.modified.contains(id);
}

void SampledDictionaryRuntimeType::for_each_updated_item(const EvaluationTime &, const void *value, void *context,
                                                         ItemIdFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (sample.added.any() || sample.removed.any()) {
        function(context, key_set_item_id);
    }
    for_each_added(sample.evaluation_time, value, context, function);
    for_each_modified(sample.evaluation_time, value, context, function);
    for_each_removed(sample.evaluation_time, value, context, function);
}

bool SampledDictionaryRuntimeType::collection_valid(const void *value) const noexcept {
    return valid_target(static_cast<const SampledDictionary *>(value)->current);
}

void SampledDictionaryRuntimeType::for_each_modified_item(const EvaluationTime &, const void *value, EvaluationTime,
                                                          void *context, ModifiedItemFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (sample.added.any() || sample.removed.any()) {
        const auto keys = item(value, key_set_item_id);
        function(context, {key_set_item_id, &keys.runtime_type, &keys.last_updated, keys.value});
    }
    for (const auto id : sample.added) {
        const auto current = item(value, id);
        function(context, {id, &current.runtime_type, &current.last_updated, current.value});
    }
    for (const auto id : sample.modified) {
        const auto current = item(value, id);
        function(context, {id, &current.runtime_type, &current.last_updated, current.value});
    }
    for (const auto id : sample.removed) {
        const auto previous = item(value, id | previous_item_flag);
        function(context, {id | previous_item_flag, &previous.runtime_type, &MIN_DT, previous.value});
    }
}

TimeSeriesRuntimeType::ConstItem TimeSeriesDictionaryReferenceRuntimeType::item(const void *value,
                                                                                TimeSeriesItemId id) const {
    if (id == key_set_item_id) {
        return TimeSeriesDictionaryRuntimeType::item(value, id);
    }

    const auto &item = representation_->item(id);
    return {item_runtime_type(), item.last_updated, &item.reference};
}

TimeSeriesRuntimeType::MutableItem TimeSeriesDictionaryReferenceRuntimeType::item(void *, TimeSeriesItemId) const {
    throw std::logic_error{"a dictionary reference representation is immutable"};
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

TimeSeriesRuntimeType::ConstItem DereferencedTimeSeriesDictionaryRuntimeType::item(const void *value,
                                                                                   TimeSeriesItemId id) const {
    if (id == key_set_item_id) {
        return TimeSeriesDictionaryRuntimeType::item(value, id);
    }

    const auto &item = representation_->item(id);
    return item;
}

TimeSeriesRuntimeType::MutableItem DereferencedTimeSeriesDictionaryRuntimeType::item(void *, TimeSeriesItemId) const {
    throw std::logic_error{"a dereferenced dictionary representation is immutable"};
}

bool DereferencedTimeSeriesDictionaryRuntimeType::item_updated(const EvaluationTime &last_updated, const void *value,
                                                               TimeSeriesItemId id) const noexcept {
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

bool DereferencedTimeSeriesDictionaryRuntimeType::item_modified(const EvaluationTime &last_updated, const void *value,
                                                                TimeSeriesItemId id) const noexcept {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    return representation_->change_time() == last_updated && representation_->modified().contains(id) &&
           !storage.key_storage().value.added().contains(id);
}

void DereferencedTimeSeriesDictionaryRuntimeType::for_each_modified_item(const EvaluationTime &last_updated,
                                                                         const void *value,
                                                                         EvaluationTime evaluation_time, void *context,
                                                                         ModifiedItemFunction function) const {
    const auto &storage = *static_cast<const time_series_dict_storage *>(value);
    const auto &keys = storage.key_storage().value;
    if (last_updated <= MIN_DT) {
        function(context, {key_set_item_id, &TimeSeriesDictionaryRuntimeType::item(value, key_set_item_id).runtime_type,
                           &MIN_DT, &storage.key_storage().value});
        for (const auto id : keys.removed()) {
            const auto current = this->item(value, id);
            function(context, {id, &current.runtime_type, &MIN_DT, current.value});
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
            const auto current = this->item(value, id);
            function(context, {id, &current.runtime_type, &current.last_updated, current.value});
        }
        for (const auto id : keys.removed()) {
            const auto current = this->item(value, id);
            function(context, {id, &current.runtime_type, &MIN_DT, current.value});
        }
    }
    if (representation_->change_time() == evaluation_time) {
        for (const auto id : representation_->modified()) {
            if (!keys.added().contains(id)) {
                const auto current = this->item(value, id);
                function(context, {id, &current.runtime_type, &current.last_updated, current.value});
            }
        }
    }
}

} // namespace

ReferenceRepresentation *
TimeSeriesDictionaryRuntimeType::create_reference_representation(ReferenceProvider &provider, ReferenceAddress input,
                                                                 const TimeSeriesType &requested_type,
                                                                 std::pmr::memory_resource &memory) const {
    const auto *dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&requested_type);
    if (dictionary == nullptr || &dictionary->key_type() != &key_runtime_type().type()) {
        return TimeSeriesRuntimeType::create_reference_representation(provider, input, requested_type, memory);
    }

    const auto *requested_reference = dynamic_cast<const TimeSeriesReferenceType *>(&dictionary->value_type());
    if (requested_reference != nullptr &&
        requested_reference->target_type().accepts(item_runtime_type().type())) {
        const auto &provided = static_cast<const TimeSeriesDictionaryType &>(type());
        const auto item_guarantee =
            strongest(TimeSeriesGuarantee::final, *provided.guarantee().value());
        if (!satisfies(item_guarantee, *requested_reference->guarantee().value())) {
            return TimeSeriesRuntimeType::create_reference_representation(provider, input, requested_type, memory);
        }

        const auto &item_type = time_series_reference_type(item_runtime_type().type(), item_guarantee);
        const auto &representation_type =
            time_series_dictionary_type(provided.key_type(), item_type, *provided.guarantee().value());
        const auto &item = resolve_time_series_runtime_type(item_type);
        auto *storage = memory.allocate(sizeof(TimeSeriesDictionaryReferenceRepresentation),
                                        alignof(TimeSeriesDictionaryReferenceRepresentation));
        auto release = util::scope_exit{[&]() noexcept {
            memory.deallocate(storage, sizeof(TimeSeriesDictionaryReferenceRepresentation),
                              alignof(TimeSeriesDictionaryReferenceRepresentation));
        }};
        auto *result = std::construct_at(static_cast<TimeSeriesDictionaryReferenceRepresentation *>(storage), provider,
                                         input, representation_type, *this, item);
        release.release();
        return result;
    }

    const auto *input_reference = dynamic_cast<const TimeSeriesReferenceType *>(&item_runtime_type().type());
    if (input_reference != nullptr && dictionary->value_type().accepts(input_reference->target_type())) {
        const auto &item = resolve_time_series_runtime_type(dictionary->value_type());
        auto *storage = memory.allocate(sizeof(DereferencedTimeSeriesDictionaryRepresentation),
                                        alignof(DereferencedTimeSeriesDictionaryRepresentation));
        auto release = util::scope_exit{[&]() noexcept {
            memory.deallocate(storage, sizeof(DereferencedTimeSeriesDictionaryRepresentation),
                              alignof(DereferencedTimeSeriesDictionaryRepresentation));
        }};
        auto *result = std::construct_at(static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(storage),
                                         provider, input, *dictionary, *this, item);
        release.release();
        return result;
    }

    return TimeSeriesRuntimeType::create_reference_representation(provider, input, requested_type, memory);
}

} // namespace bg::details
