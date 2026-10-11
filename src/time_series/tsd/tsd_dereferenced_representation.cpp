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

class DereferencedTimeSeriesDictionaryRepresentation final : public TimeSeriesDictionaryRepresentation {
  public:
    DereferencedTimeSeriesDictionaryRepresentation(ReferenceProvider &provider, Reference base_reference,
                                                   const TimeSeriesDictionaryType &type,
                                                   const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                   const TimeSeriesRuntimeType &item_runtime_type)
        : TimeSeriesDictionaryRepresentation{base_reference, type, base_runtime_type, item_runtime_type},
          provider_{&provider}, item_runtime_type_{&item_runtime_type},
          last_updated_{*ResolvedTimeSeries::resolve(base_reference).last_updated},
          item_references_{&provider.allocation().memory_resource()} {
        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference);
        base_runtime_type.for_each_item_id(base_time_series.storage, this, [](void *context, item_id_t id) {
            auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(context);
            self.add_item(id, self.last_updated_.time());
        });
    }

    ResolvedTimeSeries resolve() const override {
        return {&last_updated_, this, &dictionary_runtime_type()};
    }

    ResolvedTimeSeries base_time_series() const override { return ResolvedTimeSeries::resolve(base_reference()); }

    void notify(OutputChangeContext &context, OutputChange change) override {
        refresh_runtime_type();
        reset_changes(context.evaluation_time());

        if (change == OutputChange::invalidation) {
            invalidate(context);
            return;
        }
        if (!last_updated_.valid()) {
            revalidate(context);
            return;
        }

        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference());
        const auto input_time = base_time_series.last_updated->time();
        struct AddCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime input_time;
        } add_callback{this, input_time};
        base_runtime_type().for_each_added(*base_time_series.last_updated, base_time_series.storage, &add_callback,
                                           [](void *value, item_id_t id) {
                                               auto &callback = *static_cast<AddCallback *>(value);
                                               callback.representation->add_item(id, callback.input_time);
                                           });
        base_runtime_type().for_each_modified(
            *base_time_series.last_updated, base_time_series.storage, this, [](void *value, item_id_t id) {
                auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value);
                self.bind_item(id, self.current_base_item(id),
                               ResolvedTimeSeries::resolve(self.base_reference()).last_updated->time());
                self.modified_.set(id, self.provider_->allocation());
            });
        struct RemoveCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime evaluation_time;
        } remove_callback{this, context.evaluation_time()};
        base_runtime_type().for_each_removed(*base_time_series.last_updated, base_time_series.storage, &remove_callback,
                                             [](void *value, item_id_t id) {
                                                 auto &callback = *static_cast<RemoveCallback *>(value);
                                                 callback.representation->remove_item(id, callback.evaluation_time);
                                             });

        const auto was_valid = last_updated_.valid();
        const auto now_valid = base_time_series.valid();
        if (now_valid) {
            last_updated_.publish(input_time);
        } else if (was_valid) {
            last_updated_.invalidate(context.evaluation_time());
        }
        if (was_valid || now_valid) {
            ReferenceProvider::notify(reference(), context,
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
            return (*sample)->input();
        }
        if (id >= item_references_.size() || !item_references_[id]) {
            return {*item_runtime_type_, initial_time_series_state, nullptr};
        }

        const auto referenced_item = ResolvedTimeSeries::resolve(item_references_[id]);
        if (!referenced_item.valid()) {
            return {*item_runtime_type_, initial_time_series_state, nullptr};
        }

        return {*referenced_item.runtime_type, *referenced_item.last_updated, referenced_item.storage};
    }

    bool item_updated(const TimeSeriesState &last_updated, item_id_t id) const noexcept override {
        if (!last_updated.valid()) {
            return false;
        }
        const auto base = base_time_series();
        return base.storage != nullptr &&
               (base_runtime_type().item_updated(last_updated, base.storage, id) ||
                item_modified(last_updated, id));
    }

    bool item_modified(const TimeSeriesState &last_updated, item_id_t id) const noexcept override {
        if (id == key_set_item_id || !last_updated.valid() || change_time_ != last_updated.time()) {
            return false;
        }
        const auto base = base_time_series();
        return base.storage != nullptr && modified_.contains(id) &&
               base_runtime_type().next_item_id(last_updated, base.storage,
                                                TimeSeriesDictionaryRuntimeType::ItemRange::added, id) != id;
    }

    item_id_t next_modified_item_id(const TimeSeriesState &last_updated, item_id_t start) const noexcept override {
        if (!last_updated.valid() || change_time_ != last_updated.time()) {
            return util::occupancy_index::npos;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return util::occupancy_index::npos;
        }
        auto id = modified_.first_set(start);
        while (id != modified_.size() &&
               base_runtime_type().next_item_id(last_updated, base.storage,
                                                TimeSeriesDictionaryRuntimeType::ItemRange::added, id) == id) {
            id = modified_.first_set(id + 1);
        }
        return id == modified_.size() ? util::occupancy_index::npos : id;
    }

    void for_each_modified(const TimeSeriesState &last_updated, void *context,
                           TimeSeriesRuntimeType::ItemIdFunction function) const override {
        for (auto id = next_modified_item_id(last_updated, 0); id != util::occupancy_index::npos;
             id = next_modified_item_id(last_updated, id + 1)) {
            function(context, id);
        }
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

        base_runtime_type().for_each_updated_item(last_updated, base.storage, context, function);

        for (auto id = next_modified_item_id(last_updated, 0); id != util::occupancy_index::npos;
             id = next_modified_item_id(last_updated, id + 1)) {
            if (!base_runtime_type().item_updated(last_updated, base.storage, id)) {
                function(context, id);
            }
        }
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, EvaluationTime evaluation_time, void *context,
                                TimeSeriesRuntimeType::ModifiedItemFunction function) const override {
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return;
        }
        const auto &storage = *static_cast<const time_series_dict_storage *>(base.storage);
        const auto &keys = storage.key_storage().value;
        if (!last_updated.valid()) {
            const auto key_set = base_runtime_type().item(base.storage, key_set_item_id);
            function(context, {key_set_item_id, &key_set.runtime_type, &last_updated, key_set.value});
            for (const auto id : keys.removed()) {
                const auto previous = item(id);
                function(context, {id, &previous.runtime_type, &last_updated, previous.value});
            }
            return;
        }
        if (last_updated.time() != evaluation_time) {
            return;
        }

        if (storage.key_storage().last_updated.time() == evaluation_time) {
            const auto key_set = base_runtime_type().item(base.storage, key_set_item_id);
            function(context, {key_set_item_id, &key_set.runtime_type, &key_set.last_updated, key_set.value});
        }
        if (storage.change_time() == evaluation_time) {
            for (const auto id : keys.added()) {
                const auto current = item(id);
                function(context, {id, &current.runtime_type, &current.last_updated, current.value});
            }
            for (const auto id : keys.removed()) {
                const auto previous = item(id);
                function(context, {id, &previous.runtime_type, &initial_time_series_state, previous.value});
            }
        }
        if (change_time_ == evaluation_time) {
            for (auto id = next_modified_item_id(last_updated, 0); id != util::occupancy_index::npos;
                 id = next_modified_item_id(last_updated, id + 1)) {
                const auto current = item(id);
                function(context, {id, &current.runtime_type, &current.last_updated, current.value});
            }
        }
    }
    EvaluationTime change_time() const noexcept { return change_time_; }
    const util::index<> &modified() const noexcept { return modified_; }

  private:
    void invalidate(OutputChangeContext &context) {
        if (!last_updated_.valid()) {
            return;
        }

        for (auto id = item_id_t{}; id < item_references_.size(); ++id) {
            if (!item_references_[id]) {
                continue;
            }
            sample_switch(id, {}, context.evaluation_time());
            unsubscribe(id);
        }
        last_updated_.invalidate(context.evaluation_time());
        ReferenceProvider::notify(reference(), context, OutputChange::invalidation);
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

        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference());
        struct Callback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            EvaluationTime evaluation_time;
        } callback{this, context.evaluation_time()};
        base_runtime_type().for_each_item_id(base_time_series.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            self.add_item(id, callback.evaluation_time);
            self.modified_.set(id, self.provider_->allocation());
        });

        last_updated_.publish(context.evaluation_time());
        ReferenceProvider::notify(reference(), context, OutputChange::revalidation);
    }

    TimeSeriesRuntimeType::ConstItem current_base_item(item_id_t id) const {
        const auto base = base_time_series();
        return base_runtime_type().item(base.storage, id);
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
        const auto previous =
            previous_reference ? ResolvedTimeSeries::resolve(previous_reference) : ResolvedTimeSeries{};

        auto new_reference = base_item.last_updated.valid() ? base_item.runtime_type.reference(base_item.value)
                                                            : Reference{};
        if (new_reference) {
            new_reference = ReferenceProvider::representation(new_reference, *item_runtime_type_);
        }
        if (new_reference == previous_reference) {
            return;
        }

        unsubscribe(id);
        const auto current = new_reference ? ResolvedTimeSeries::resolve(new_reference) : ResolvedTimeSeries{};
        sample_switch(id, current, sample_time, previous);
        item_references_[id] = new_reference;
        if (new_reference) {
            ReferenceProvider::add_item_notification(new_reference, provider_->owner(), this, id,
                                                     &notify_item_change_callback, &publish_item_changes_callback);
        }
    }

    void sample_switch(item_id_t id, ResolvedTimeSeries current, EvaluationTime evaluation_time,
                       ResolvedTimeSeries previous = {}) {
        auto previous_last_updated = TimeSeriesState{};
        if (auto *existing = samples_.get(id, sample_positions_)) {
            previous = (*existing)->previous;
            previous_last_updated = *previous.last_updated;
            previous.last_updated = &previous_last_updated;
            (*existing)->release();
            samples_.erase_at(id, provider_->allocation());
            sample_positions_.reset(id);
        }

        if (previous.runtime_type == nullptr && id < item_references_.size() && item_references_[id]) {
            previous = ResolvedTimeSeries::resolve(item_references_[id]);
        }
        auto *sample = SampledDictionary::create(previous, current, evaluation_time, provider_->allocation());
        if (sample == nullptr) {
            return;
        }
        auto destroy_sample = util::scope_exit{[&]() noexcept { sample->release(); }};
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

    static void clear_samples_callback(void *value, EvaluationContext &) noexcept {
        static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value)->clear_samples();
    }

    void clear_samples() noexcept {
        auto &memory = provider_->allocation();
        for (const auto id : sample_positions_) {
            samples_[id]->release();
        }
        samples_.release(sample_positions_, memory);
        sample_positions_.release(memory);
        sample_cleanup_registered_ = false;
    }

    void unsubscribe(item_id_t id) noexcept {
        if (id < item_references_.size() && item_references_[id]) {
            ReferenceProvider::remove_item_notification(item_references_[id], provider_->owner(), this, id,
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
                                         ? ResolvedTimeSeries::resolve(item_references_[id])
                                         : ResolvedTimeSeries{};
        if (change == OutputChange::invalidation) {
            auto previous = referenced_item;
            previous.last_updated = &last_updated_;
            sample_switch(id, {}, context.evaluation_time(), previous);
        } else if (change == OutputChange::revalidation) {
            auto previous = referenced_item;
            previous.last_updated = &initial_time_series_state;
            previous.storage = nullptr;
            sample_switch(id, referenced_item, context.evaluation_time(), previous);
        }
        modified_.set(id, provider_->allocation());
    }

    void publish_item_changes(OutputChangeContext &context) {
        if (last_updated_.time() == context.evaluation_time()) {
            return;
        }

        const auto was_valid = last_updated_.valid();
        last_updated_.publish(context.evaluation_time());
        ReferenceProvider::notify(reference(), context,
                                  was_valid ? OutputChange::update : OutputChange::revalidation);
    }

    void reset_changes(EvaluationTime evaluation_time) {
        if (change_time_ == evaluation_time) {
            return;
        }
        modified_.clear();
        change_time_ = evaluation_time;
    }

    ReferenceProvider *provider_;
    const TimeSeriesRuntimeType *item_runtime_type_;
    TimeSeriesState last_updated_;
    std::pmr::vector<Reference> item_references_;
    util::occupancy_index sample_positions_;
    util::sparse_bunch<SampledDictionary *> samples_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
    bool sample_cleanup_registered_{};

    const TimeSeriesDictionaryRuntimeType &base_runtime_type() const noexcept {
        const auto base = base_time_series();
        return static_cast<const TimeSeriesDictionaryRuntimeType &>(*base.runtime_type);
    }
};

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
