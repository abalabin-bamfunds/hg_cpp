#include "tsd_reference_representation.hpp"

#include <bg/allocator.hpp>
#include "tsd_reference_sample.hpp"

#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <memory>
#include <vector>

namespace bg::details {
namespace {

class DereferencedTimeSeriesDictionaryRepresentation;

class DereferencedTimeSeriesDictionaryRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    DereferencedTimeSeriesDictionaryRuntimeType(const TimeSeriesDictionaryType &type,
                                                const TimeSeriesDictionaryRuntimeType &base,
                                                const TimeSeriesRuntimeType &item,
                                                DereferencedTimeSeriesDictionaryRepresentation &representation)
        : TimeSeriesDictionaryRuntimeType{type, base.key_runtime_type(), item}, representation_{&representation} {}

    ConstItem item(const void *value, item_id_t id) const override;
    MutableItem item(void *, item_id_t) const override;
    bool item_updated(const EvaluationTime &last_updated, const void *value,
                      item_id_t id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified(const EvaluationTime &last_updated, const void *value, void *context,
                           ItemIdFunction function) const override;
    bool item_modified(const EvaluationTime &last_updated, const void *value,
                       item_id_t id) const noexcept override;
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

  private:
    DereferencedTimeSeriesDictionaryRepresentation *representation_;
};

class DereferencedTimeSeriesDictionaryRepresentation final : public ReferenceRepresentation {
  public:
    DereferencedTimeSeriesDictionaryRepresentation(ReferenceProvider &provider, Reference base_reference,
                                                   const TimeSeriesDictionaryType &type,
                                                   const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                   const TimeSeriesRuntimeType &item_runtime_type)
        : ReferenceRepresentation{base_reference}, provider_{&provider}, base_runtime_type_{&base_runtime_type},
          runtime_type_{type, base_runtime_type, item_runtime_type, *this},
          last_updated_{*resolve_reference(base_reference).last_updated},
          item_references_{&provider.allocation().memory_resource()} {
        const auto base_time_series = resolve_reference(base_reference);
        base_runtime_type.for_each_item_id(base_time_series.storage, this, [](void *context, item_id_t id) {
            auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(context);
            self.add_item(id, self.last_updated_);
        });
    }

    ResolvedTimeSeries resolve() const override {
        return {&last_updated_, resolve_reference(base_reference()).storage, &runtime_type_};
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

        const auto base_time_series = resolve_reference(base_reference());
        const auto input_time = *base_time_series.last_updated;
        struct AddCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime input_time;
        } add_callback{this, input_time};
        base_runtime_type_->for_each_added(input_time, base_time_series.storage, &add_callback,
                                           [](void *value, item_id_t id) {
                                               auto &callback = *static_cast<AddCallback *>(value);
                                               callback.representation->add_item(id, callback.input_time);
                                           });
        base_runtime_type_->for_each_modified(
            input_time, base_time_series.storage, this, [](void *value, item_id_t id) {
                auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value);
                self.bind_item(id, self.current_base_item(id), *resolve_reference(self.base_reference()).last_updated);
                self.modified_.set(id, self.provider_->allocation());
            });
        struct RemoveCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime evaluation_time;
        } remove_callback{this, context.evaluation_time()};
        base_runtime_type_->for_each_removed(input_time, base_time_series.storage, &remove_callback,
                                             [](void *value, item_id_t id) {
                                                 auto &callback = *static_cast<RemoveCallback *>(value);
                                                 callback.representation->remove_item(id, callback.evaluation_time);
                                             });

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = input_time;
        const auto now_valid = last_updated_ > MIN_DT;
        if (was_valid || now_valid) {
            notify_reference(reference(), context,
                             now_valid ? (was_valid ? OutputChange::update : OutputChange::revalidation)
                                       : OutputChange::invalidation);
        }
    }

    void destroy(allocator &memory) noexcept override {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
        }
        for (auto id = item_id_t{}; id < item_references_.size(); ++id) {
            unsubscribe(id);
        }
        clear_samples();
        modified_.release(memory);
        memory.delete_object(this);
    }

    TimeSeriesRuntimeType::ConstItem item(item_id_t id) const {
        if (const auto *sample = samples_.get(id, sample_positions_)) {
            return dictionary_sample_item(**sample);
        }
        if (id >= item_references_.size() || !item_references_[id]) {
            return {runtime_type_.item_runtime_type(), MIN_DT, nullptr};
        }

        const auto referenced_item = resolve_reference(item_references_[id]);
        if (!referenced_item.valid()) {
            return {runtime_type_.item_runtime_type(), MIN_DT, nullptr};
        }

        const auto base_item = current_base_item(id);
        const auto &visible_time = base_item.last_updated > *referenced_item.last_updated
                                       ? base_item.last_updated
                                       : *referenced_item.last_updated;
        return {*referenced_item.runtime_type, visible_time, referenced_item.storage};
    }
    EvaluationTime change_time() const noexcept { return change_time_; }
    const util::index<> &modified() const noexcept { return modified_; }

  private:
    void invalidate(OutputChangeContext &context) {
        if (last_updated_ <= MIN_DT) {
            return;
        }

        for (auto id = item_id_t{}; id < item_references_.size(); ++id) {
            if (!item_references_[id]) {
                continue;
            }
            sample_switch(id, {}, context.evaluation_time());
            unsubscribe(id);
        }
        last_updated_ = MIN_DT;
        notify_reference(reference(), context, OutputChange::invalidation);
    }

    void revalidate(OutputChangeContext &context) {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
            clear_samples();
        }
        for (auto id = item_id_t{}; id < item_references_.size(); ++id) {
            unsubscribe(id);
        }
        item_references_.clear();
        modified_.clear();

        const auto base_time_series = resolve_reference(base_reference());
        struct Callback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime evaluation_time;
        } callback{this, context.evaluation_time()};
        base_runtime_type_->for_each_item_id(base_time_series.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            self.add_item(id, callback.evaluation_time);
            self.modified_.set(id, self.provider_->allocation());
        });

        last_updated_ = context.evaluation_time();
        notify_reference(reference(), context, OutputChange::revalidation);
    }

    TimeSeriesRuntimeType::ConstItem current_base_item(item_id_t id) const {
        return base_runtime_type_->item(resolve_reference(base_reference()).storage, id);
    }

    void add_item(item_id_t id, EvaluationTime sample_time) {
        if (item_references_.size() <= id) {
            item_references_.resize(static_cast<std::size_t>(id) + 1);
        }
        bind_item(id, current_base_item(id), sample_time);
    }

    void remove_item(item_id_t id, EvaluationTime evaluation_time) {
        if (id >= item_references_.size() || !item_references_[id]) {
            return;
        }

        sample_switch(id, {}, evaluation_time);
        unsubscribe(id);
        modified_.reset(id);
    }

    void bind_item(item_id_t id, TimeSeriesRuntimeType::ConstItem base_item, EvaluationTime sample_time) {
        const auto previous_reference = item_references_[id];
        const auto previous = previous_reference ? resolve_reference(previous_reference) : ResolvedTimeSeries{};

        auto new_reference = base_item.last_updated > MIN_DT ? base_item.runtime_type.reference(base_item.value)
                                                           : Reference{};
        if (new_reference) {
            new_reference = reference_representation(new_reference, runtime_type_.item_runtime_type().type());
        }
        if (new_reference == previous_reference) {
            return;
        }

        unsubscribe(id);
        const auto current = new_reference ? resolve_reference(new_reference) : ResolvedTimeSeries{};
        sample_switch(id, current, sample_time, previous);
        item_references_[id] = new_reference;
        if (new_reference) {
            add_reference_item_notification(new_reference, provider_->owner(), this, id, &notify_item_change_callback,
                                            &publish_item_changes_callback);
        }
    }

    void sample_switch(item_id_t id, ResolvedTimeSeries current, EvaluationTime evaluation_time,
                       ResolvedTimeSeries previous = {}) {
        auto previous_last_updated = MIN_DT;
        if (auto *existing = samples_.get(id, sample_positions_)) {
            previous = dictionary_sample_previous(**existing);
            previous_last_updated = *previous.last_updated;
            previous.last_updated = &previous_last_updated;
            destroy_dictionary_sample(*existing);
            samples_.erase_at(id, provider_->allocation());
            sample_positions_.reset(id);
        }

        if (previous.runtime_type == nullptr && id < item_references_.size() && item_references_[id]) {
            previous = resolve_reference(item_references_[id]);
        }
        auto *sample = make_dictionary_sample(previous, current, evaluation_time, provider_->allocation());
        if (sample == nullptr) {
            return;
        }
        auto destroy_sample = util::scope_exit{[&]() noexcept { destroy_dictionary_sample(sample); }};
        if (!sample_cleanup_registered_) {
            provider_->add_cycle_callback(this, clear_samples_callback);
            sample_cleanup_registered_ = true;
        }

        sample_positions_.set(id, provider_->allocation());
        auto reset_position = util::scope_exit{[&]() noexcept { sample_positions_.reset(id); }};
        samples_.emplace_at(id, provider_->allocation(), sample);
        reset_position.release();
        destroy_sample.release();
    }

    static void clear_samples_callback(void *value) noexcept {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value)->clear_samples();
    }

    void clear_samples() noexcept {
        auto &memory = provider_->allocation();
        for (const auto id : sample_positions_) {
            destroy_dictionary_sample(samples_[id]);
        }
        samples_.release(sample_positions_, memory);
        sample_positions_.release(memory);
        sample_cleanup_registered_ = false;
    }

    void unsubscribe(item_id_t id) noexcept {
        if (id < item_references_.size() && item_references_[id]) {
            remove_reference_item_notification(item_references_[id], provider_->owner(), this, id,
                                               &notify_item_change_callback);
        }
        if (id < item_references_.size()) {
            item_references_[id] = nullptr;
        }
    }

    static void notify_item_change_callback(OutputChangeContext &context, void *destination_representation,
                                             item_id_t id,
                                            OutputChange change) {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(destination_representation)
            ->notify_item_change(context, id, change);
    }

    static void publish_item_changes_callback(OutputChangeContext &context, void *destination_representation) {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(destination_representation)
            ->publish_item_changes(context);
    }

    void notify_item_change(OutputChangeContext &context, item_id_t id, OutputChange change) {
        reset_changes(context.evaluation_time());
        const auto referenced_item = id < item_references_.size() && item_references_[id]
                                         ? resolve_reference(item_references_[id])
                                         : ResolvedTimeSeries{};
        if (change == OutputChange::invalidation) {
            auto previous = referenced_item;
            previous.last_updated = &last_updated_;
            sample_switch(id, {}, context.evaluation_time(), previous);
        } else if (change == OutputChange::revalidation) {
            auto previous = referenced_item;
            previous.last_updated = &MIN_DT;
            previous.storage = nullptr;
            sample_switch(id, referenced_item, context.evaluation_time(), previous);
        }
        modified_.set(id, provider_->allocation());
    }

    void publish_item_changes(OutputChangeContext &context) {
        if (last_updated_ == context.evaluation_time()) {
            return;
        }

        const auto was_valid = last_updated_ > MIN_DT;
        last_updated_ = context.evaluation_time();
        notify_reference(reference(), context, was_valid ? OutputChange::update : OutputChange::revalidation);
    }

    void reset_changes(EvaluationTime evaluation_time) {
        if (change_time_ == evaluation_time) {
            return;
        }
        modified_.clear();
        change_time_ = evaluation_time;
    }

    ReferenceProvider *provider_;
    const TimeSeriesDictionaryRuntimeType *base_runtime_type_;
    DereferencedTimeSeriesDictionaryRuntimeType runtime_type_;
    EvaluationTime last_updated_;
    std::pmr::vector<Reference> item_references_;
    util::occupancy_index sample_positions_;
    util::sparse_bunch<SampledDictionary *> samples_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
    bool sample_cleanup_registered_{};
};

TimeSeriesRuntimeType::ConstItem DereferencedTimeSeriesDictionaryRuntimeType::item(const void *value,
                                                                                   item_id_t id) const {
    if (id == key_set_item_id) {
        return TimeSeriesDictionaryRuntimeType::item(value, id);
    }

    const auto &item = representation_->item(id);
    return item;
}

TimeSeriesRuntimeType::MutableItem DereferencedTimeSeriesDictionaryRuntimeType::item(void *, item_id_t) const {
    throw std::logic_error{"a dereferenced dictionary representation is immutable"};
}

bool DereferencedTimeSeriesDictionaryRuntimeType::item_updated(const EvaluationTime &last_updated, const void *value,
                                                               item_id_t id) const noexcept {
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
                                                                item_id_t id) const noexcept {
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

ReferenceRepresentation *make_dereferenced_tsd_representation(ReferenceProvider &provider,
                                                              Reference base_reference,
                                                              const TimeSeriesDictionaryType &type,
                                                              const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                              const TimeSeriesRuntimeType &item_runtime_type,
                                                              allocator &memory) {
    return memory.new_object<DereferencedTimeSeriesDictionaryRepresentation>(
        provider, base_reference, type, base_runtime_type, item_runtime_type);
}

} // namespace bg::details
