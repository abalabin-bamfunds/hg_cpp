#include "tsd_reference_representation.hpp"

#include <bg/allocator.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>

#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <memory>
#include <stdexcept>
#include <vector>

namespace bg::details {
namespace {

template <bool Recursive> struct ItemBindingState;

template <> struct ItemBindingState<false> {
    explicit ItemBindingState(allocator &memory) : references{&memory.memory_resource()} {}

    // Item resolution may release deferred reference state when its target advances.
    mutable std::pmr::vector<ReferenceInputStorage> references;
};

template <> struct ItemBindingState<true> {
    explicit ItemBindingState(allocator &memory)
        : references{&memory.memory_resource()}, retired_times{&memory.memory_resource()} {}

    std::pmr::vector<Reference> references;
    util::occupancy_index source_positions;
    util::sparse_bunch<ItemReference> sources;
    util::occupancy_index retired_positions;
    std::pmr::vector<EvaluationTime> retired_times;
};

template <bool Recursive> class DereferencedTimeSeriesDictionaryRepresentation final
    : public TimeSeriesDictionaryRepresentation {
  public:
    DereferencedTimeSeriesDictionaryRepresentation(ReferenceProvider &provider, Reference base_reference,
                                                   const TimeSeriesDictionaryType &type,
                                                   const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                   const TimeSeriesRuntimeType &item_runtime_type)
        : TimeSeriesDictionaryRepresentation{base_reference, type, base_runtime_type, item_runtime_type},
          provider_{&provider}, item_runtime_type_{&item_runtime_type},
          bindings_{provider.allocation()} {
        auto rollback = util::scope_exit{[this]() noexcept { rollback_initialization(); }};
        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference);
        last_updated_ = base_time_series.last_updated;
        struct Callback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            const ResolvedTimeSeries *base_time_series;
        } callback{this, &base_time_series};
        if (base_time_series.storage != nullptr) {
            base_runtime_type.for_each_item_id(base_time_series.storage, &callback, [](void *context, item_id_t id) {
                auto &callback = *static_cast<Callback *>(context);
                callback.representation->add_item(id, callback.representation->last_updated_.time(),
                                                  *callback.base_time_series);
            });
        }
        rollback.release();
    }

    ResolvedTimeSeries resolve() const override {
        return {last_updated_, this, &dictionary_runtime_type()};
    }

    ResolvedTimeSeries base_time_series() const override { return ResolvedTimeSeries::resolve(base_reference()); }

    void notify(OutputChangeContext &context, OutputChange change) override {
        refresh_runtime_type();
        reset_changes(context.evaluation_time());

        if constexpr (Recursive) {
            provider_->begin_item_notification_batch();
        }
        auto cancel_batch = util::scope_exit{[&]() noexcept {
            if constexpr (Recursive) {
                provider_->cancel_item_notification_batch();
            }
        }};
        const auto publish_batch = [&]() {
            if constexpr (Recursive) {
                provider_->publish_item_notification_batch(context);
                cancel_batch.release();
            }
        };

        if (change == OutputChange::invalidation) {
            invalidate(context);
            publish_batch();
            return;
        }
        if (!last_updated_.valid()) {
            revalidate(context);
            publish_batch();
            return;
        }

        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference());
        const auto input_time = base_time_series.last_updated.time();
        const auto &input_runtime_type = base_runtime_type(base_time_series);
        struct AddCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            OutputChangeContext *context;
            const ResolvedTimeSeries *base_time_series;
            EvaluationTime input_time;
        } add_callback{this, &context, &base_time_series, input_time};
        input_runtime_type.for_each_added(base_time_series.last_updated, base_time_series.storage, &add_callback,
                                           [](void *value, item_id_t id) {
                                               auto &callback = *static_cast<AddCallback *>(value);
                                               if constexpr (Recursive) {
                                                   callback.representation->add_item(
                                                       id, callback.input_time, *callback.base_time_series,
                                                       callback.context);
                                               } else {
                                                   callback.representation->add_item(
                                                       id, callback.input_time, *callback.base_time_series);
                                                   callback.representation->modified_.set(
                                                       id, callback.representation->provider_->allocation());
                                               }
                                           });
        input_runtime_type.for_each_updated(
            base_time_series.last_updated, base_time_series.storage, &add_callback, [](void *value, item_id_t id) {
                auto &callback = *static_cast<AddCallback *>(value);
                auto &self = *callback.representation;
                const auto item = self.current_base_item(id, *callback.base_time_series);
                if constexpr (Recursive) {
                    self.update_recursive_item(id, item, *callback.context);
                } else {
                    self.bind_item(id, item, callback.input_time);
                    self.modified_.set(id, self.provider_->allocation());
                }
            });
        struct RemoveCallback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            OutputChangeContext *context;
            EvaluationTime evaluation_time;
        } remove_callback{this, &context, context.evaluation_time()};
        input_runtime_type.for_each_removed(base_time_series.last_updated, base_time_series.storage, &remove_callback,
                                            [](void *value, item_id_t id) {
                                                auto &callback = *static_cast<RemoveCallback *>(value);
                                                if constexpr (Recursive) {
                                                    callback.representation->remove_item(
                                                        id, callback.evaluation_time, *callback.context);
                                                } else {
                                                    callback.representation->remove_item(id,
                                                                                         callback.evaluation_time,
                                                                                         *callback.context);
                                                }
                                            });

        if constexpr (Recursive) {
            if (input_runtime_type.item_updated(base_time_series.last_updated, base_time_series.storage,
                                                key_set_item_id)) {
                const auto was_valid = last_updated_.valid();
                last_updated_.publish(input_time);
                ReferenceProvider::notify(reference(), context,
                                          was_valid ? OutputChange::update : OutputChange::revalidation);
            }
        } else {
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
        publish_batch();
    }

    void detach() noexcept override {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
            sample_cleanup_registered_ = false;
        }
        for (auto id = item_id_t{}; id < bindings_.references.size(); ++id) {
            unsubscribe(id);
        }
    }

    void destroy(allocator &memory) noexcept override {
        clear_samples();
        if constexpr (Recursive) {
            release_all_sources();
        }
        modified_.release(memory);
        memory.delete_object(this);
    }

    TimeSeriesRuntimeType::ConstItem item(item_id_t id) const {
        if (const auto *sample = samples_.get(id, sample_positions_)) {
            return sample->input();
        }
        if (id >= bindings_.references.size()) {
            return {*item_runtime_type_, initial_time_series_state, nullptr};
        }

        if constexpr (Recursive) {
            const auto reference = bindings_.references[id];
            if (!reference) {
                return {*item_runtime_type_, initial_time_series_state, nullptr};
            }
            const auto referenced_item = ResolvedTimeSeries::resolve(reference);
            if (referenced_item.runtime_type == nullptr) {
                return {*item_runtime_type_, initial_time_series_state, nullptr};
            }
            return {*referenced_item.runtime_type, referenced_item.last_updated, referenced_item.storage};
        } else {
            const auto base = base_time_series();
            if (base.storage == nullptr) {
                return {*item_runtime_type_, initial_time_series_state, nullptr};
            }
            const auto base_item = base_runtime_type(base).item(base.storage, id);
            const auto reference = bindings_.references[id].reference();
            const auto referenced_item = reference ? ResolvedTimeSeries::resolve(reference) : ResolvedTimeSeries{};
            const auto &runtime_type = referenced_item.runtime_type == nullptr ? *item_runtime_type_
                                                                               : *referenced_item.runtime_type;
            return {runtime_type, bindings_.references[id].observed_state(base_item.last_updated, referenced_item),
                    referenced_item.storage};
        }
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
        return runtime_type.item_updated(last_updated, base.storage, id) || is_updated_item(last_updated, id, base);
    }

    bool is_updated_item(const TimeSeriesState &last_updated, item_id_t id) const noexcept override {
        return is_updated_item(last_updated, id, base_time_series());
    }

    bool is_modified_item(const TimeSeriesState &last_updated, item_id_t id) const noexcept override {
        if (id == key_set_item_id || !last_updated.valid() || change_time_ != last_updated.time()) {
            return false;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return false;
        }
        return modified_.contains(id);
    }

    item_id_t next_updated_item_id(const TimeSeriesState &last_updated, item_id_t start) const noexcept override {
        if (!last_updated.valid() || change_time_ != last_updated.time()) {
            return util::occupancy_index::npos;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return util::occupancy_index::npos;
        }
        return next_updated_item_id(last_updated, start, base);
    }

    item_id_t next_modified_item_id(const TimeSeriesState &last_updated, item_id_t start) const noexcept override {
        if (!last_updated.valid() || change_time_ != last_updated.time()) {
            return util::occupancy_index::npos;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return util::occupancy_index::npos;
        }
        return next_modified_item_id(last_updated, start, base);
    }

    void for_each_updated(const TimeSeriesState &last_updated, void *context,
                          TimeSeriesRuntimeType::ItemIdFunction function) const override {
        if (!last_updated.valid() || change_time_ != last_updated.time()) {
            return;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return;
        }
        for (auto id = next_updated_item_id(last_updated, 0, base); id != util::occupancy_index::npos;
             id = next_updated_item_id(last_updated, id + 1, base)) {
            function(context, id);
        }
    }

    void for_each_modified(const TimeSeriesState &last_updated, void *context,
                           TimeSeriesRuntimeType::ItemIdFunction function) const override {
        if (!last_updated.valid() || change_time_ != last_updated.time()) {
            return;
        }
        const auto base = base_time_series();
        if (base.storage == nullptr) {
            return;
        }

        for (const auto id : modified_) {
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
        const auto &runtime_type = base_runtime_type(base);

        if constexpr (Recursive) {
            if (runtime_type.item_updated(last_updated, base.storage, key_set_item_id)) {
                function(context, key_set_item_id);
            }
            runtime_type.for_each_added(last_updated, base.storage, context, function);
            runtime_type.for_each_removed(last_updated, base.storage, context, function);
        } else {
            runtime_type.for_each_updated_item(last_updated, base.storage, context, function);
        }

        for (auto id = modified_.first_set(0); id != modified_.size(); id = modified_.first_set(id + 1)) {
            const auto structural =
                runtime_type.item_in_range(last_updated, base.storage,
                                           TimeSeriesDictionaryRuntimeType::ItemRange::added, id) ||
                runtime_type.item_in_range(last_updated, base.storage,
                                           TimeSeriesDictionaryRuntimeType::ItemRange::removed, id);
            if constexpr (Recursive) {
                if (!structural) {
                    function(context, id);
                }
            } else if (!runtime_type.item_updated(last_updated, base.storage, id)) {
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
        const auto &runtime_type = base_runtime_type(base);
        const auto &keys = storage.key_storage().value;
        if constexpr (Recursive) {
            struct Callback {
                const DereferencedTimeSeriesDictionaryRepresentation *representation;
                void *context;
                TimeSeriesRuntimeType::ModifiedItemFunction function;
            } callback{this, context, function};
            if (!last_updated.valid()) {
                const auto key_set = runtime_type.item(base.storage, key_set_item_id);
                function(context, {key_set_item_id, &key_set.runtime_type, &last_updated, key_set.value});
                runtime_type.for_each_item_id(base.storage, &callback, [](void *value, item_id_t id) {
                    auto &callback = *static_cast<Callback *>(value);
                    const auto current = callback.representation->item(id);
                    callback.function(callback.context,
                                      {id, &current.runtime_type, &initial_time_series_state, current.value});
                });
                for (const auto id : keys.removed()) {
                    const auto previous = item(id);
                    function(context, {id, &previous.runtime_type, &last_updated, previous.value});
                }
                return;
            }
            if (last_updated.time() != evaluation_time) {
                return;
            }
            const auto key_set = runtime_type.item(base.storage, key_set_item_id);
            if (key_set.last_updated.time() == evaluation_time) {
                function(context, {key_set_item_id, &key_set.runtime_type, &key_set.last_updated, key_set.value});
            }
            runtime_type.for_each_added(last_updated, base.storage, &callback, [](void *value, item_id_t id) {
                auto &callback = *static_cast<Callback *>(value);
                const auto current = callback.representation->item(id);
                callback.function(callback.context,
                                  {id, &current.runtime_type, &current.last_updated, current.value});
            });
            runtime_type.for_each_removed(last_updated, base.storage, &callback, [](void *value, item_id_t id) {
                auto &callback = *static_cast<Callback *>(value);
                const auto previous = callback.representation->item(id);
                callback.function(callback.context,
                                  {id, &previous.runtime_type, &initial_time_series_state, previous.value});
            });
            if (change_time_ == evaluation_time) {
                for (auto id = next_updated_item_id(last_updated, 0, base); id != util::occupancy_index::npos;
                     id = next_updated_item_id(last_updated, id + 1, base)) {
                    const auto current = item(id);
                    function(context, {id, &current.runtime_type, &current.last_updated, current.value});
                }
            }
            return;
        }

        if (!last_updated.valid()) {
            const auto key_set = runtime_type.item(base.storage, key_set_item_id);
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
            const auto key_set = runtime_type.item(base.storage, key_set_item_id);
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
            for (auto id = next_updated_item_id(last_updated, 0, base); id != util::occupancy_index::npos;
                 id = next_updated_item_id(last_updated, id + 1, base)) {
                const auto current = item(id);
                function(context, {id, &current.runtime_type, &current.last_updated, current.value});
            }
        }
    }
    EvaluationTime change_time() const noexcept { return change_time_; }
    const util::index<> &modified() const noexcept { return modified_; }

  private:
    bool is_updated_item(const TimeSeriesState &last_updated, item_id_t id,
                         const ResolvedTimeSeries &base) const noexcept {
        if (id == key_set_item_id || !last_updated.valid() || change_time_ != last_updated.time() ||
            base.storage == nullptr || !modified_.contains(id)) {
            return false;
        }
        const auto &runtime_type = base_runtime_type(base);
        return !runtime_type.item_in_range(last_updated, base.storage,
                                           TimeSeriesDictionaryRuntimeType::ItemRange::added, id) &&
               !runtime_type.item_in_range(last_updated, base.storage,
                                           TimeSeriesDictionaryRuntimeType::ItemRange::removed, id);
    }

    item_id_t next_updated_item_id(const TimeSeriesState &last_updated, item_id_t start,
                                   const ResolvedTimeSeries &base) const noexcept {
        if (!last_updated.valid() || change_time_ != last_updated.time() || base.storage == nullptr) {
            return util::occupancy_index::npos;
        }
        const auto &runtime_type = base_runtime_type(base);
        auto id = modified_.first_set(start);
        while (id != modified_.size() &&
               (runtime_type.item_in_range(last_updated, base.storage,
                                           TimeSeriesDictionaryRuntimeType::ItemRange::added, id) ||
                runtime_type.item_in_range(last_updated, base.storage,
                                           TimeSeriesDictionaryRuntimeType::ItemRange::removed, id))) {
            id = modified_.first_set(id + 1);
        }
        return id == modified_.size() ? util::occupancy_index::npos : id;
    }

    item_id_t next_modified_item_id(const TimeSeriesState &last_updated, item_id_t start,
                                    const ResolvedTimeSeries &base) const noexcept {
        if (!last_updated.valid() || change_time_ != last_updated.time() || base.storage == nullptr) {
            return util::occupancy_index::npos;
        }

        const auto id = modified_.first_set(start);
        return id == modified_.size() ? util::occupancy_index::npos : id;
    }

    void invalidate(OutputChangeContext &context) {
        if (!last_updated_.valid()) {
            return;
        }

        if constexpr (Recursive) {
            for (const auto id : bindings_.source_positions) {
                notify_recursive_source(id, context, OutputChange::invalidation);
            }
        } else {
            for (auto id = item_id_t{}; id < bindings_.references.size(); ++id) {
                if (!bindings_.references[id].reference()) {
                    continue;
                }
                sample_switch(id, {}, context.evaluation_time());
                unsubscribe(id);
            }
        }
        last_updated_.invalidate(context.evaluation_time());
        ReferenceProvider::notify(reference(), context, OutputChange::invalidation);
    }

    void revalidate(OutputChangeContext &context) {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
            sample_cleanup_registered_ = false;
            clear_samples();
        }
        if constexpr (Recursive) {
            release_retired(context.evaluation_time());
        }
        modified_.clear();

        const auto base_time_series = ResolvedTimeSeries::resolve(base_reference());
        if constexpr (Recursive) {
            last_updated_.publish(context.evaluation_time());
            for (const auto id : bindings_.source_positions) {
                notify_recursive_source(id, context, OutputChange::revalidation);
            }
        } else {
            for (auto id = item_id_t{}; id < bindings_.references.size(); ++id) {
                unsubscribe(id);
            }
            bindings_.references.clear();
        }
        struct Callback {
            DereferencedTimeSeriesDictionaryRepresentation *representation;
            const ResolvedTimeSeries *base_time_series;
            EvaluationTime evaluation_time;
            OutputChangeContext *context;
        } callback{this, &base_time_series, context.evaluation_time(), &context};
        base_runtime_type(base_time_series).for_each_item_id(base_time_series.storage, &callback,
                                                              [](void *value, item_id_t id) {
            auto &callback = *static_cast<Callback *>(value);
            auto &self = *callback.representation;
            self.add_item(id, callback.evaluation_time, *callback.base_time_series, callback.context);
            if constexpr (!Recursive) {
                self.modified_.set(id, self.provider_->allocation());
            }
        });

        if constexpr (!Recursive) {
            last_updated_.publish(context.evaluation_time());
        }
        ReferenceProvider::notify(reference(), context, OutputChange::revalidation);
    }

    TimeSeriesRuntimeType::ConstItem current_base_item(item_id_t id, const ResolvedTimeSeries &base) const {
        return base_runtime_type(base).item(base.storage, id);
    }

    void add_item(item_id_t id, EvaluationTime sample_time, const ResolvedTimeSeries &base,
                  OutputChangeContext *context = nullptr) {
        if (bindings_.references.size() <= id) {
            bindings_.references.resize(static_cast<std::size_t>(id) + 1);
        }
        if constexpr (Recursive) {
            if (bindings_.source_positions.contains(id)) {
                restore_item(id);
                if (context != nullptr) {
                    notify_recursive_source(id, *context, OutputChange::revalidation);
                }
                return;
            }
        }
        bind_item(id, current_base_item(id, base), sample_time);
    }

    void remove_item(item_id_t id, EvaluationTime evaluation_time, OutputChangeContext &context) {
        if (id >= bindings_.references.size()) {
            return;
        }

        if constexpr (Recursive) {
            if (!bindings_.references[id]) {
                return;
            }
            notify_recursive_source(id, context, OutputChange::invalidation);
            retire_item(id, evaluation_time);
            modified_.reset(id);
            return;
        } else {
            (void)context;
            modified_.reset(id);
            if (!bindings_.references[id].reference()) {
                return;
            }

            sample_switch(id, {}, evaluation_time);
            unsubscribe(id);
        }
    }

    void bind_item(item_id_t id, TimeSeriesRuntimeType::ConstItem base_item, EvaluationTime sample_time) {
        if constexpr (Recursive) {
            (void)base_item;
            (void)sample_time;
            if (bindings_.source_positions.contains(id)) {
                throw std::logic_error{"a recursive TSD item source is already bound"};
            }
            auto &memory = provider_->allocation();
            auto &source = bindings_.sources.emplace_at(
                id, memory.memory_resource(), base_reference(), id);
            auto position_registered = false;
            auto rollback = util::scope_exit{[&]() noexcept {
                unsubscribe(id);
                if (id < bindings_.references.size()) {
                    bindings_.references[id] = nullptr;
                }
                if (!provider_->release_item_reference(source)) {
                    std::terminate();
                }
                bindings_.sources.erase_at(id, memory.memory_resource());
                if (position_registered) {
                    bindings_.source_positions.reset(id);
                }
            }};
            if (!bindings_.source_positions.set(id, memory)) {
                throw std::logic_error{"a recursive TSD item source is already bound"};
            }
            position_registered = true;
            bindings_.references[id] = ReferenceProvider::representation(Reference{&source}, *item_runtime_type_);
            subscribe_item(id);
            rollback.release();
            return;
        } else {
            auto &selected_reference = bindings_.references[id];
            const auto previous_reference = selected_reference.reference();
            const auto previous =
                previous_reference ? ResolvedTimeSeries::resolve(previous_reference) : ResolvedTimeSeries{};
            const auto previous_view_was_valid = previous_reference && previous.last_updated.valid_before(sample_time);

            auto new_reference = base_item.last_updated.valid() ? base_item.runtime_type.reference(base_item.value)
                                                                : Reference{};
            if (new_reference) {
                new_reference = ReferenceProvider::representation(new_reference, *item_runtime_type_);
            }
            if (new_reference == previous_reference) {
                selected_reference.set(previous_reference, previous_view_was_valid);
                return;
            }

            unsubscribe(id);
            const auto current = new_reference ? ResolvedTimeSeries::resolve(new_reference) : ResolvedTimeSeries{};
            sample_switch(id, current, sample_time, previous);
            selected_reference.set(new_reference, previous_view_was_valid);
            if (new_reference) {
                ReferenceProvider::add_item_notification(new_reference, provider_->owner(), this, id,
                                                         &notify_item_change_callback, &publish_item_changes_callback);
            }
        }
    }

    void update_recursive_item(item_id_t id, TimeSeriesRuntimeType::ConstItem base_item,
                               OutputChangeContext &context) {
        if (!bindings_.source_positions.contains(id)) {
            return;
        }
        const auto change = base_item.last_updated.became_invalid()
                                ? OutputChange::invalidation
                                : base_item.last_updated.became_valid() ? OutputChange::revalidation
                                                                       : OutputChange::update;
        notify_recursive_source(id, context, change);
    }

    void notify_recursive_source(item_id_t id, OutputChangeContext &context, OutputChange change) {
        ReferenceProvider::notify(Reference{&bindings_.sources[id]}, context, change);
    }

    void retire_item(item_id_t id, EvaluationTime evaluation_time) {
        if (bindings_.retired_positions.contains(id)) {
            return;
        }
        if (bindings_.retired_times.size() <= id) {
            bindings_.retired_times.resize(static_cast<std::size_t>(id) + 1);
        }
        bindings_.retired_times[id] = evaluation_time;
        bindings_.retired_positions.set(id, provider_->allocation());
        if (!sample_cleanup_registered_) {
            provider_->add_cycle_callback(this, clear_samples_callback);
            sample_cleanup_registered_ = true;
        }
    }

    void restore_item(item_id_t id) { bindings_.retired_positions.reset(id); }

    void release_retired(EvaluationTime evaluation_time) noexcept {
        auto &memory = provider_->allocation();
        for (const auto id : bindings_.retired_positions) {
            if (bindings_.retired_times[id] >= evaluation_time) {
                continue;
            }

            const auto item_reference = bindings_.references[id];
            unsubscribe(id);
            auto &source = bindings_.sources[id];
            if (!provider_->release_item_reference(source)) {
                bindings_.references[id] = item_reference;
                subscribe_item(id);
                continue;
            }
            bindings_.sources.erase_at(id, memory.memory_resource());
            bindings_.source_positions.reset(id);
            bindings_.retired_positions.reset(id);
        }
        if (!bindings_.retired_positions.empty() && !sample_cleanup_registered_) {
            provider_->add_cycle_callback(this, clear_samples_callback);
            sample_cleanup_registered_ = true;
        }
    }

    void release_all_sources() noexcept {
        auto &memory = provider_->allocation();
        for (const auto id : bindings_.source_positions) {
            unsubscribe(id);
            if (!provider_->release_item_reference(bindings_.sources[id])) {
                std::terminate();
            }
            bindings_.sources.erase_at(id, memory.memory_resource());
            bindings_.source_positions.reset(id);
        }
        bindings_.sources.release(bindings_.source_positions, memory.memory_resource());
        bindings_.source_positions.release(memory);
        bindings_.retired_positions.release(memory);
    }

    void rollback_initialization() noexcept {
        detach();
        clear_samples();
        if constexpr (Recursive) {
            release_all_sources();
        }
        modified_.release(provider_->allocation());
    }

    void sample_switch(item_id_t id, ResolvedTimeSeries current, EvaluationTime evaluation_time,
                       ResolvedTimeSeries previous = {}) {
        if (auto *existing = samples_.get(id, sample_positions_)) {
            previous = existing->previous();
            existing->release();
            samples_.erase_at(id, provider_->allocation());
            sample_positions_.reset(id);
        }

        if (previous.runtime_type == nullptr && id < bindings_.references.size()) {
            if constexpr (Recursive) {
                if (bindings_.references[id]) {
                    previous = ResolvedTimeSeries::resolve(bindings_.references[id]);
                }
            } else if (const auto reference = bindings_.references[id].reference()) {
                previous = ResolvedTimeSeries::resolve(reference);
            }
        }
        const auto sample = SampledReferenceChange::create(*item_runtime_type_, previous, current,
                                                           evaluation_time, provider_->allocation());
        if (!sample) {
            return;
        }
        auto release_sample = util::scope_exit{[&]() noexcept { sample.release(); }};
        if (!sample_cleanup_registered_) {
            provider_->add_cycle_callback(this, clear_samples_callback);
            sample_cleanup_registered_ = true;
        }

        sample_positions_.set(id, provider_->allocation());
        auto reset_position = util::scope_exit{[&]() noexcept { sample_positions_.reset(id); }};
        samples_.emplace_at(id, provider_->allocation(), sample);
        reset_position.release();
        release_sample.release();
    }

    static void clear_samples_callback(void *value, EvaluationContext &context) noexcept {
        auto &self = *static_cast<DereferencedTimeSeriesDictionaryRepresentation *>(value);
        self.clear_samples();
        if constexpr (Recursive) {
            self.release_retired(context.evaluation_time());
        }
    }

    void clear_samples() noexcept {
        auto &memory = provider_->allocation();
        for (const auto id : sample_positions_) {
            samples_[id].release();
        }
        samples_.release(sample_positions_, memory);
        sample_positions_.release(memory);
        sample_cleanup_registered_ = false;
    }

    void unsubscribe(item_id_t id) noexcept {
        if (id >= bindings_.references.size()) {
            return;
        }

        if constexpr (Recursive) {
            const auto reference = bindings_.references[id];
            if (reference) {
                ReferenceProvider::remove_item_notification(reference, provider_->owner(), this, id,
                                                            &notify_item_change_callback);
            }
            bindings_.references[id] = nullptr;
        } else {
            auto &selected_reference = bindings_.references[id];
            if (const auto reference = selected_reference.reference()) {
                ReferenceProvider::remove_item_notification(reference, provider_->owner(), this, id,
                                                            &notify_item_change_callback);
            }
            selected_reference.set({}, selected_reference.selected_view_was_valid());
        }
    }

    void subscribe_item(item_id_t id) {
        auto reference = Reference{};
        if constexpr (Recursive) {
            reference = bindings_.references[id];
        } else {
            reference = bindings_.references[id].reference();
        }
        if (reference) {
            ReferenceProvider::add_item_notification(reference, provider_->owner(), this, id,
                                                     &notify_item_change_callback,
                                                     &publish_item_changes_callback);
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
        auto reference = Reference{};
        if (id < bindings_.references.size()) {
            if constexpr (Recursive) {
                reference = bindings_.references[id];
            } else {
                reference = bindings_.references[id].reference();
            }
        }
        const auto referenced_item = reference ? ResolvedTimeSeries::resolve(reference) : ResolvedTimeSeries{};
        if (change == OutputChange::invalidation) {
            auto previous = referenced_item;
            previous.last_updated = last_updated_;
            sample_switch(id, {}, context.evaluation_time(), previous);
        } else if (change == OutputChange::revalidation) {
            auto previous = referenced_item;
            previous.last_updated = initial_time_series_state;
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
    ItemBindingState<Recursive> bindings_;
    util::occupancy_index sample_positions_;
    util::sparse_bunch<SampledReferenceChange> samples_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
    bool sample_cleanup_registered_{};

    const TimeSeriesDictionaryRuntimeType &base_runtime_type(const ResolvedTimeSeries &base) const noexcept {
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
    return memory.new_object<DereferencedTimeSeriesDictionaryRepresentation<false>>(
        provider, base_reference, type, base_runtime_type, item_runtime_type);
}

ReferenceRepresentation *make_recursive_tsd_representation(ReferenceProvider &provider, Reference base_reference,
                                                           const TimeSeriesDictionaryType &type,
                                                           const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                           const TimeSeriesRuntimeType &item_runtime_type,
                                                           allocator &memory) {
    return memory.new_object<DereferencedTimeSeriesDictionaryRepresentation<true>>(
        provider, base_reference, type, base_runtime_type, item_runtime_type);
}

} // namespace bg::details
