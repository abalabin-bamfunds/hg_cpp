#include "fixed_reference_representation.hpp"

#include <bg/allocator.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/util/index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/static_storage.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {
namespace {

enum class FieldMode : std::uint8_t { direct, routed_reference, dereferenced, adapted };

struct Field {
    FieldMode mode;
    std::size_t state_index;
    const TimeSeriesRuntimeType *runtime_type;
};

class FixedCollectionRepresentation;

struct DereferencedItem {
    Reference reference;
    TimeSeriesState last_updated;
    TimeSeriesState output_last_updated;
    const void *storage{};
    const TimeSeriesRuntimeType *runtime_type{};
    SampledReferenceChange sample;
};

struct AdaptedItem {
    ItemReference source;
    Reference representation;
};

static_assert(sizeof(AdaptedItem) == 4 * sizeof(void *));

class FixedCollectionRepresentationRuntimeType final : public TimeSeriesRuntimeType {
  public:
    FixedCollectionRepresentationRuntimeType(const TimeSeriesType &type, std::vector<Field> fields)
        : TimeSeriesRuntimeType{type, 0, 1, false, false, false}, fields_{std::move(fields)} {}

    ItemLayout item_layout(item_id_t) const override {
        throw std::logic_error{"a fixed collection reference representation has no packed output layout"};
    }

    const TimeSeriesRuntimeType &item_runtime_type(item_id_t id) const override { return *fields_.at(id).runtime_type; }

    std::size_t item_count(const void *) const override { return fields_.size(); }

    ConstItem item(const void *value, item_id_t id) const override;

    MutableItem item(void *, item_id_t) const override {
        throw std::logic_error{"a fixed collection reference representation is immutable"};
    }

    void for_each_item_id(const void *, void *context, ItemIdFunction function) const override {
        for (auto id = item_id_t{}; id < fields_.size(); ++id) {
            function(context, id);
        }
    }

    bool all_valid(const TimeSeriesState &last_updated, const void *value) const noexcept override;

    bool all_valid(const TimeSeriesState &last_updated, const void *value,
                   const TimeSeriesType &declared_type) const noexcept override;

    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t id) const override;

    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;

    bool mark_item_modified(void *, item_id_t, EvaluationTime, bool, allocator &) const
        noexcept override {
        std::terminate();
    }

    bool collection_valid(const void *value) const noexcept override;

    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override;

    void copy_tick(const TimeSeriesInputData &, OutputState &) const override {
        throw std::logic_error{"a fixed collection reference representation cannot be an output"};
    }

    void copy(const TimeSeriesInputData &, OutputState &) const override {
        throw std::logic_error{"a fixed collection reference representation cannot be an output"};
    }

    void construct_initial_value(void *) const noexcept override { std::terminate(); }

    void destroy_value(void *, allocator &) const noexcept override { std::terminate(); }

    const Field &field(item_id_t id) const noexcept { return fields_[id]; }

    bool matches(const std::vector<Field> &fields) const noexcept {
        if (fields_.size() != fields.size()) {
            return false;
        }
        for (auto id = std::size_t{}; id < fields.size(); ++id) {
            if (fields_[id].mode != fields[id].mode || fields_[id].runtime_type != fields[id].runtime_type) {
                return false;
            }
        }
        return true;
    }

    const ReferenceRepresentationFactory *reference_representation_factory() const noexcept override {
        return fixed_collection_reference_representation_factory();
    }

  private:
    const FixedCollectionRepresentation &representation(const void *value) const noexcept;
    bool fields_all_valid(const TimeSeriesState &last_updated, const void *value,
                          std::size_t field_count) const noexcept;

    std::vector<Field> fields_;
};

class FixedCollectionRepresentation final : public ReferenceRepresentation {
  public:
    FixedCollectionRepresentation(ReferenceProvider &provider, Reference base_reference,
                                 const FixedCollectionRepresentationRuntimeType &runtime_type)
        : ReferenceRepresentation{base_reference}, provider_{&provider}, runtime_type_{&runtime_type},
          routed_items_{&provider.allocation().memory_resource()},
          adapted_items_{&provider.allocation().memory_resource()},
          dereferenced_items_{&provider.allocation().memory_resource()} {
        const auto base = ResolvedTimeSeries::resolve(base_reference);
        last_updated_ = base.last_updated;
        auto release_slots = util::scope_exit{[&]() noexcept {
            for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
                const auto &field = runtime_type_->field(id);
                if (field.mode == FieldMode::dereferenced && field.state_index < dereferenced_items_.size()) {
                    auto &item = dereferenced_items_[field.state_index];
                    unsubscribe(item.reference, id);
                    item.sample.release();
                } else if (field.mode == FieldMode::adapted && field.state_index < adapted_items_.size()) {
                    auto &item = adapted_items_[field.state_index];
                    unsubscribe(item.representation, id);
                    if (!provider.release_item_reference(item.source)) {
                        std::terminate();
                    }
                }
            }
        }};
        initialize_slots(base);
        const auto evaluation_time = provider.owner().evaluation_time();
        update_routed_reference_state(evaluation_time);
        if (any_items_valid() && !last_updated_.valid()) {
            last_updated_.publish(evaluation_time);
        } else if (!any_items_valid() && last_updated_.valid()) {
            last_updated_.invalidate(evaluation_time);
        }
        release_slots.release();
    }

    ResolvedTimeSeries resolve() const override { return {last_updated_, this, runtime_type_}; }

    void notify(OutputChangeContext &context, OutputChange change) override {
        reset_changes(context.evaluation_time());
        update_routed_reference_state(context.evaluation_time());
        provider_->begin_item_notification_batch();
        auto cancel_batch = util::scope_exit{[&]() noexcept { provider_->cancel_item_notification_batch(); }};
        if (change == OutputChange::invalidation) {
            invalidate(context);
        } else if (!last_updated_.valid()) {
            revalidate(context);
        } else {
            const auto base = ResolvedTimeSeries::resolve(base_reference());
            bool changed = false;
            for (auto id = item_id_t{}; id < runtime_type_->item_count(base.storage); ++id) {
                if (!base.runtime_type->item_updated(base.last_updated, base.storage, id)) {
                    continue;
                }

                const auto &field = runtime_type_->field(id);
                if (field.mode == FieldMode::routed_reference) {
                    ReferenceProvider::notify_item_bindings(reference(), id, context);
                    const auto source_item = base.runtime_type->item(base.storage, id);
                    const auto item_change = source_item.last_updated.became_valid()
                                                 ? OutputChange::revalidation
                                                 : source_item.last_updated.became_invalid()
                                                       ? OutputChange::invalidation
                                                       : OutputChange::update;
                    provider_->notify_subscriptions(routed_items_[field.state_index].subscriptions(), context,
                                                    item_change);
                    continue;
                }
                if (field.mode == FieldMode::direct) {
                    if (field.runtime_type->is_reference()) {
                        ReferenceProvider::notify_item_bindings(reference(), id, context);
                    }
                    modified_.set(id, provider_->allocation());
                    changed = true;
                    continue;
                }
                if (field.mode == FieldMode::adapted) {
                    const auto source_item = base.runtime_type->item(base.storage, id);
                    const auto item_change = source_item.last_updated.became_valid()
                                                 ? OutputChange::revalidation
                                                 : source_item.last_updated.became_invalid()
                                                       ? OutputChange::invalidation
                                                       : OutputChange::update;
                    notify_adapted_source(context, id, item_change);
                    continue;
                }
                changed |= rebind(id, field, base, context.evaluation_time());
            }
            if (changed) {
                publish(context, context.evaluation_time());
            }
        }
        provider_->publish_item_notification_batch(context);
        cancel_batch.release();
    }

    void detach() noexcept override {
        if (sample_cleanup_registered_) {
            provider_->remove_cycle_callback(this);
            sample_cleanup_registered_ = false;
        }
        for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
            const auto &field = runtime_type_->field(id);
            if (field.mode == FieldMode::dereferenced) {
                auto &item = dereferenced_items_[field.state_index];
                unsubscribe(item.reference, id);
                item.reference = {};
                continue;
            }
            if (field.mode != FieldMode::adapted) {
                continue;
            }
            auto &item = adapted_items_[field.state_index];
            unsubscribe(item.representation, id);
            item.representation = {};
        }
    }

    void destroy(allocator &memory) noexcept override {
        for (auto &item : dereferenced_items_) {
            item.sample.release();
        }
        for (auto &item : adapted_items_) {
            if (!provider_->release_item_reference(item.source)) {
                std::terminate();
            }
        }
        for (auto &item : routed_items_) {
            if (!provider_->release_item_reference(item)) {
                std::terminate();
            }
        }
        modified_.release(memory);
        memory.delete_object(this);
    }

    TimeSeriesRuntimeType::ConstItem item(item_id_t id) const {
        const auto &field = runtime_type_->field(id);
        const auto base = ResolvedTimeSeries::resolve(base_reference());
        if (field.mode == FieldMode::direct) {
            if (base.storage == nullptr) {
                return {*field.runtime_type, initial_time_series_state, nullptr};
            }
            return base.runtime_type->item(base.storage, id);
        }
        if (field.mode == FieldMode::routed_reference) {
            return {*field.runtime_type, routed_reference_state_, &routed_items_[field.state_index]};
        }
        if (field.mode == FieldMode::adapted) {
            const auto target = ResolvedTimeSeries::resolve(adapted_items_[field.state_index].representation);
            if (target.runtime_type == nullptr) {
                return {*field.runtime_type, initial_time_series_state, nullptr};
            }
            return {*target.runtime_type, target.last_updated, target.storage};
        }

        const auto &dereferenced = dereferenced_items_[field.state_index];
        if (dereferenced.sample) {
            return dereferenced.sample.input();
        }
        if (!dereferenced.output_last_updated.valid() || !dereferenced.reference) {
            return {*field.runtime_type, dereferenced.output_last_updated, nullptr};
        }
        const auto target = ResolvedTimeSeries::resolve(dereferenced.reference);
        if (target.runtime_type == nullptr) {
            return {*field.runtime_type, dereferenced.output_last_updated, nullptr};
        }
        return {*target.runtime_type, dereferenced.output_last_updated, target.storage};
    }

    bool item_updated(const TimeSeriesState &last_updated, item_id_t id) const noexcept {
        if (!last_updated.valid() || id >= runtime_type_->item_count(nullptr)) {
            return false;
        }
        const auto &field = runtime_type_->field(id);
        if (field.mode == FieldMode::routed_reference) {
            return routed_reference_state_.became_valid() &&
                   routed_reference_state_.time() == last_updated.time();
        }
        if (item_modified(last_updated, id)) {
            return true;
        }
        if (field.mode != FieldMode::direct) {
            return false;
        }
        const auto base = ResolvedTimeSeries::resolve(base_reference());
        return base.storage != nullptr && base.runtime_type->item_updated(last_updated, base.storage, id);
    }

    bool item_modified(const TimeSeriesState &last_updated, item_id_t id) const noexcept {
        return last_updated.valid() && change_time_ == last_updated.time() && modified_.contains(id);
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, void *context,
                               TimeSeriesRuntimeType::ItemIdFunction function) const {
        for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
            if (item_updated(last_updated, id)) {
                function(context, id);
            }
        }
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, EvaluationTime evaluation_time, void *context,
                                TimeSeriesRuntimeType::ModifiedItemFunction function) const {
        auto publish_item = [&](item_id_t id) {
            const auto current = item(id);
            function(context, {id, &current.runtime_type, &current.last_updated, current.value});
        };
        if (!last_updated.valid()) {
            for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
                publish_item(id);
            }
            return;
        }
        if (last_updated.time() != evaluation_time) {
            return;
        }
        for_each_updated_item(last_updated, &publish_item, [](void *value, item_id_t id) {
            (*static_cast<decltype(publish_item) *>(value))(id);
        });
    }

    bool collection_valid() const noexcept {
        for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
            if (item(id).last_updated.valid()) {
                return true;
            }
        }
        return false;
    }

  private:
    void initialize_slots(const ResolvedTimeSeries &base) {
        auto routed_count = std::size_t{};
        auto adapted_count = std::size_t{};
        auto dereferenced_count = std::size_t{};
        for (auto id = item_id_t{}; id < runtime_type_->item_count(base.storage); ++id) {
            const auto mode = runtime_type_->field(id).mode;
            routed_count += mode == FieldMode::routed_reference;
            adapted_count += mode == FieldMode::adapted;
            dereferenced_count += mode == FieldMode::dereferenced;
        }
        routed_items_.reserve(routed_count);
        adapted_items_.reserve(adapted_count);
        dereferenced_items_.reserve(dereferenced_count);
        for (auto id = item_id_t{}; id < runtime_type_->item_count(base.storage); ++id) {
            const auto &field = runtime_type_->field(id);
            if (field.mode == FieldMode::routed_reference) {
                routed_items_.emplace_back(base_reference(), id);
                continue;
            }
            if (field.mode == FieldMode::adapted) {
                adapted_items_.emplace_back();
                adapted_items_.back().source = ItemReference{base_reference(), id};
                bind(adapted_items_.back(), id, field);
                continue;
            }
            if (field.mode == FieldMode::dereferenced) {
                dereferenced_items_.emplace_back();
                bind(dereferenced_items_.back(), id, field, base, MIN_DT);
            }
        }
    }

    bool rebind(item_id_t id, const Field &field, const ResolvedTimeSeries &base, EvaluationTime evaluation_time) {
        auto &target = dereferenced_items_[field.state_index];
        const auto previous_reference = target.reference;
        const auto previous = snapshot(target);
        auto new_reference = Reference{};
        const auto source_item = base.runtime_type->item(base.storage, id);
        if (source_item.last_updated.valid() && source_item.value != nullptr) {
            new_reference = source_item.runtime_type.reference(source_item.value);
        }
        if (new_reference) {
            new_reference = ReferenceProvider::representation(new_reference, *field.runtime_type);
        }
        if (new_reference == previous_reference) {
            refresh_snapshot(target);
            update_output_state(target, target.last_updated.valid(), evaluation_time);
            modified_.set(id, provider_->allocation());
            return true;
        }

        unsubscribe(target.reference, id);
        target.reference = new_reference;
        if (new_reference) {
            ReferenceProvider::add_item_notification(new_reference, provider_->owner(), this, id,
                                                     &notify_target_callback, &publish_item_changes_callback);
        }
        const auto current = new_reference ? ResolvedTimeSeries::resolve(new_reference) : ResolvedTimeSeries{};
        replace_sample(target, *field.runtime_type, previous, current, evaluation_time);
        refresh_snapshot(target);
        update_output_state(target, current.valid(), evaluation_time);
        modified_.set(id, provider_->allocation());
        return true;
    }

    void bind(DereferencedItem &target, item_id_t id, const Field &field, const ResolvedTimeSeries &base,
              EvaluationTime evaluation_time) {
        unsubscribe(target.reference, id);
        target.reference = {};
        auto reference = Reference{};
        if (base.storage != nullptr) {
            const auto source_item = base.runtime_type->item(base.storage, id);
            if (source_item.last_updated.valid() && source_item.value != nullptr) {
                reference = source_item.runtime_type.reference(source_item.value);
            }
        }
        if (reference) {
            reference = ReferenceProvider::representation(reference, *field.runtime_type);
        }
        target.reference = reference;
        if (reference) {
            ReferenceProvider::add_item_notification(reference, provider_->owner(), this, id,
                                                     &notify_target_callback, &publish_item_changes_callback);
        }
        refresh_snapshot(target);
        const auto current = target.reference ? ResolvedTimeSeries::resolve(target.reference) : ResolvedTimeSeries{};
        if (evaluation_time == MIN_DT) {
            target.output_last_updated = current.runtime_type == nullptr ? initial_time_series_state
                                                                         : current.last_updated;
        } else {
            update_output_state(target, current.valid(), evaluation_time);
        }
    }

    void bind(AdaptedItem &target, item_id_t id, const Field &field) {
        unsubscribe(target.representation, id);
        auto reference = ReferenceProvider::representation(Reference{&target.source}, *field.runtime_type);
        target.representation = reference;
        ReferenceProvider::add_item_notification(reference, provider_->owner(), this, id,
                                                 &notify_target_callback, &publish_item_changes_callback);
    }

    static ResolvedTimeSeries snapshot(const DereferencedItem &item) noexcept {
        return {item.last_updated, item.storage, item.runtime_type};
    }

    static void refresh_snapshot(DereferencedItem &item) noexcept {
        if (!item.reference) {
            item.last_updated = initial_time_series_state;
            item.storage = nullptr;
            item.runtime_type = nullptr;
            return;
        }
        const auto target = ResolvedTimeSeries::resolve(item.reference);
        if (target.runtime_type != nullptr) {
            item.last_updated = target.last_updated;
            item.storage = target.storage;
            item.runtime_type = target.runtime_type;
            return;
        }
        item.last_updated = initial_time_series_state;
        item.storage = nullptr;
        item.runtime_type = nullptr;
    }

    void replace_sample(DereferencedItem &item, const TimeSeriesRuntimeType &runtime_type,
                        ResolvedTimeSeries previous,
                        ResolvedTimeSeries current,
                        EvaluationTime evaluation_time) {
        auto replacement = SampledReferenceChange{};
        if (item.sample) {
            previous = item.sample.previous();
        }
        if (!previous.valid() && !current.valid()) {
            item.sample.release();
            item.sample = {};
            return;
        }
        replacement = SampledReferenceChange::create(runtime_type, previous, current, evaluation_time,
                                                     provider_->allocation());
        item.sample.release();
        item.sample = {};
        if (replacement) {
            item.sample = replacement;
            if (!sample_cleanup_registered_) {
                provider_->add_cycle_callback(this, &clear_samples_callback);
                sample_cleanup_registered_ = true;
            }
        }
    }

    static void update_output_state(DereferencedItem &item, bool valid, EvaluationTime evaluation_time) noexcept {
        item.output_last_updated.update_collection_validity(evaluation_time, valid);
    }

    void notify_target(OutputChangeContext &context, item_id_t id, OutputChange change) {
        reset_changes(context.evaluation_time());
        const auto &field = runtime_type_->field(id);
        if (field.mode == FieldMode::adapted) {
            modified_.set(id, provider_->allocation());
            return;
        }
        if (field.mode != FieldMode::dereferenced) {
            return;
        }
        auto &target = dereferenced_items_[field.state_index];
        const auto previous = snapshot(target);
        const auto current = target.reference ? ResolvedTimeSeries::resolve(target.reference) : ResolvedTimeSeries{};
        if (change == OutputChange::invalidation) {
            replace_sample(target, *field.runtime_type, previous, {}, context.evaluation_time());
        } else if (change == OutputChange::revalidation) {
            replace_sample(target, *field.runtime_type, {}, current, context.evaluation_time());
        }
        refresh_snapshot(target);
        update_output_state(target, current.valid(), context.evaluation_time());
        modified_.set(id, provider_->allocation());
    }

    bool any_items_valid() const noexcept {
        for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
            if (item(id).last_updated.valid()) {
                return true;
            }
        }
        return false;
    }

    void update_routed_reference_state(EvaluationTime evaluation_time) noexcept {
        if (routed_reference_state_.valid()) {
            return;
        }
        routed_reference_state_.publish(evaluation_time);
    }

    void publish(OutputChangeContext &context, EvaluationTime time) {
        const auto valid = any_items_valid();
        const auto was_valid = last_updated_.valid();
        last_updated_.update_collection_validity(time, valid);
        if (!valid && !was_valid) {
            return;
        }
        const auto change = valid ? (was_valid ? OutputChange::update : OutputChange::revalidation)
                                  : OutputChange::invalidation;
        ReferenceProvider::notify(reference(), context, change);
    }

    void invalidate(OutputChangeContext &context) {
        auto changed = false;
        for (auto id = item_id_t{}; id < runtime_type_->item_count(nullptr); ++id) {
            const auto &field = runtime_type_->field(id);
            if (field.mode == FieldMode::routed_reference) {
                const auto base = ResolvedTimeSeries::resolve(base_reference());
                if (base.storage == nullptr || !base.runtime_type->item(base.storage, id).last_updated.valid()) {
                    provider_->notify_subscriptions(routed_items_[field.state_index].subscriptions(), context,
                                                    OutputChange::invalidation);
                }
                continue;
            } else if (field.mode == FieldMode::direct) {
                modified_.set(id, provider_->allocation());
                changed = true;
                continue;
            } else if (field.mode == FieldMode::dereferenced || field.mode == FieldMode::adapted) {
                changed = true;
                if (field.mode == FieldMode::dereferenced) {
                    auto &target = dereferenced_items_[field.state_index];
                    const auto previous = snapshot(target);
                    replace_sample(target, *field.runtime_type, previous, {}, context.evaluation_time());
                    if (target.output_last_updated.valid()) {
                        target.output_last_updated.invalidate(context.evaluation_time());
                    }
                    modified_.set(id, provider_->allocation());
                    unsubscribe(target.reference, id);
                    target.reference = {};
                    target.last_updated = initial_time_series_state;
                    target.storage = nullptr;
                    target.runtime_type = nullptr;
                } else {
                    auto &target = adapted_items_[field.state_index];
                    notify_adapted_source(context, id, OutputChange::invalidation);
                    unsubscribe(target.representation, id);
                }
            }
        }
        if (changed) {
            publish(context, context.evaluation_time());
        }
    }

    void revalidate(OutputChangeContext &context) {
        const auto base = ResolvedTimeSeries::resolve(base_reference());
        if (base.runtime_type == nullptr || base.storage == nullptr) {
            return;
        }
        update_routed_reference_state(context.evaluation_time());
        auto changed = false;
        for (auto id = item_id_t{}; id < runtime_type_->item_count(base.storage); ++id) {
            const auto &field = runtime_type_->field(id);
            if (field.mode == FieldMode::routed_reference) {
                if (base.runtime_type->item(base.storage, id).last_updated.valid()) {
                    provider_->notify_subscriptions(routed_items_[field.state_index].subscriptions(), context,
                                                    OutputChange::revalidation);
                }
                continue;
            } else if (field.mode == FieldMode::direct) {
                if (field.runtime_type->is_reference()) {
                    ReferenceProvider::notify_item_bindings(reference(), id, context);
                }
                modified_.set(id, provider_->allocation());
                changed = true;
            } else if (field.mode == FieldMode::dereferenced) {
                auto &target = dereferenced_items_[field.state_index];
                auto previous_last_updated = target.last_updated;
                auto previous = snapshot(target);
                previous.last_updated = previous_last_updated;
                bind(target, id, field, base, context.evaluation_time());
                const auto current = target.reference ? ResolvedTimeSeries::resolve(target.reference)
                                                      : ResolvedTimeSeries{};
                replace_sample(target, *field.runtime_type, previous, current, context.evaluation_time());
                modified_.set(id, provider_->allocation());
                changed = true;
            } else {
                auto &target = adapted_items_[field.state_index];
                bind(target, id, field);
                const auto source_item = base.runtime_type->item(base.storage, id);
                notify_adapted_source(context, id, source_item.last_updated.valid()
                                                       ? OutputChange::revalidation
                                                       : OutputChange::invalidation);
                modified_.set(id, provider_->allocation());
                changed = true;
            }
        }
        if (changed) {
            publish(context, context.evaluation_time());
        }
    }

    void reset_changes(EvaluationTime evaluation_time) {
        if (change_time_ == evaluation_time) {
            return;
        }
        modified_.clear();
        change_time_ = evaluation_time;
    }

    void unsubscribe(Reference reference, item_id_t id) noexcept {
        if (reference) {
            ReferenceProvider::remove_item_notification(reference, provider_->owner(), this, id,
                                                        &notify_target_callback);
        }
    }

    void notify_adapted_source(OutputChangeContext &context, item_id_t id, OutputChange change) {
        const auto &field = runtime_type_->field(id);
        ReferenceProvider::notify(Reference{&adapted_items_[field.state_index].source}, context, change);
    }

    static void notify_target_callback(OutputChangeContext &context, void *representation, item_id_t id,
                                       OutputChange change) {
        static_cast<FixedCollectionRepresentation *>(representation)->notify_target(context, id, change);
    }

    static void publish_item_changes_callback(OutputChangeContext &context, void *representation) {
        auto &self = *static_cast<FixedCollectionRepresentation *>(representation);
        self.update_routed_reference_state(context.evaluation_time());
        self.publish(context, context.evaluation_time());
    }

    static void clear_samples_callback(void *representation, EvaluationContext &) noexcept {
        auto &self = *static_cast<FixedCollectionRepresentation *>(representation);
        for (auto &item : self.dereferenced_items_) {
            item.sample.release();
            item.sample = {};
        }
        self.sample_cleanup_registered_ = false;
    }

    ReferenceProvider *provider_;
    const FixedCollectionRepresentationRuntimeType *runtime_type_;
    TimeSeriesState last_updated_;
    TimeSeriesState routed_reference_state_;
    std::pmr::vector<ItemReference> routed_items_;
    std::pmr::vector<AdaptedItem> adapted_items_;
    std::pmr::vector<DereferencedItem> dereferenced_items_;
    util::index<> modified_;
    EvaluationTime change_time_{MIN_DT};
    bool sample_cleanup_registered_{};
};

const FixedCollectionRepresentation &FixedCollectionRepresentationRuntimeType::representation(
    const void *value) const noexcept {
    return *static_cast<const FixedCollectionRepresentation *>(value);
}

TimeSeriesRuntimeType::ConstItem FixedCollectionRepresentationRuntimeType::item(const void *value,
                                                                                item_id_t id) const {
    return representation(value).item(id);
}

bool FixedCollectionRepresentationRuntimeType::all_valid(const TimeSeriesState &last_updated,
                                                          const void *value) const noexcept {
    return fields_all_valid(last_updated, value, fields_.size());
}

bool FixedCollectionRepresentationRuntimeType::all_valid(const TimeSeriesState &last_updated, const void *value,
                                                          const TimeSeriesType &declared_type) const noexcept {
    const auto *declared_list = type_cast<TimeSeriesListType>(&declared_type);
    const auto *declared_bundle = type_cast<TimeSeriesBundleType>(&declared_type);
    if (declared_list == nullptr && declared_bundle == nullptr) {
        return TimeSeriesRuntimeType::all_valid(last_updated, value, declared_type);
    }
    const auto field_count = declared_list != nullptr ? declared_list->item_count() : declared_bundle->item_count();
    if (field_count == std::dynamic_extent) {
        return TimeSeriesRuntimeType::all_valid(last_updated, value, declared_type);
    }
    return fields_all_valid(last_updated, value, field_count);
}

bool FixedCollectionRepresentationRuntimeType::item_updated(const TimeSeriesState &last_updated, const void *value,
                                                             item_id_t id) const {
    return representation(value).item_updated(last_updated, id);
}

void FixedCollectionRepresentationRuntimeType::for_each_updated_item(const TimeSeriesState &last_updated,
                                                                      const void *value, void *context,
                                                                      ItemIdFunction function) const {
    representation(value).for_each_updated_item(last_updated, context, function);
}

bool FixedCollectionRepresentationRuntimeType::collection_valid(const void *value) const noexcept {
    return representation(value).collection_valid();
}

void FixedCollectionRepresentationRuntimeType::for_each_modified_item(
    const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time, void *context,
    ModifiedItemFunction function) const {
    representation(value).for_each_modified_item(last_updated, evaluation_time, context, function);
}

bool FixedCollectionRepresentationRuntimeType::fields_all_valid(const TimeSeriesState &last_updated,
                                                                 const void *value,
                                                                 std::size_t field_count) const noexcept {
    if (!last_updated.valid() || value == nullptr || field_count > fields_.size()) {
        return false;
    }
    const auto &view = representation(value);
    for (auto id = std::size_t{}; id < field_count; ++id) {
        if (!view.item(static_cast<item_id_t>(id)).last_updated.valid()) {
            return false;
        }
    }
    return true;
}

bool is_fixed_collection(const TimeSeriesRuntimeType &runtime_type) noexcept {
    return type_cast<TimeSeriesListType>(&runtime_type.type()) != nullptr ||
           type_cast<TimeSeriesBundleType>(&runtime_type.type()) != nullptr;
}

bool representation_required(const TimeSeriesRuntimeType &provided,
                             const TimeSeriesRuntimeType &requested) noexcept {
    if (!is_fixed_collection(provided) || !is_fixed_collection(requested)) {
        return false;
    }
    const auto &provided_type = static_cast<const FixedItemTimeSeriesType &>(provided.type());
    const auto &requested_type = static_cast<const FixedItemTimeSeriesType &>(requested.type());
    if (provided_type.item_count() < requested_type.item_count()) {
        return false;
    }
    for (auto id = std::size_t{}; id < requested_type.item_count(); ++id) {
        const auto &provided_item = provided.item_runtime_type(static_cast<item_id_t>(id));
        const auto &requested_item = requested.item_runtime_type(static_cast<item_id_t>(id));
        if (provided_item.is_reference() != requested_item.is_reference() ||
            ReferenceProvider::representation_required(provided_item, requested_item)) {
            return true;
        }
    }
    return false;
}

const TimeSeriesRuntimeType &representation_runtime_type(const TimeSeriesRuntimeType &provided,
                                                         const TimeSeriesRuntimeType &requested) {
    const auto &requested_type = static_cast<const FixedItemTimeSeriesType &>(requested.type());
    auto fields = std::vector<Field>{};
    fields.reserve(requested_type.item_count());
    auto routed_count = std::size_t{};
    auto dereferenced_count = std::size_t{};
    auto adapted_count = std::size_t{};
    for (auto id = std::size_t{}; id < requested_type.item_count(); ++id) {
        const auto &provided_item = provided.item_runtime_type(static_cast<item_id_t>(id));
        const auto &requested_item = requested.item_runtime_type(static_cast<item_id_t>(id));
        if (!provided_item.is_reference() && requested_item.is_reference()) {
            const auto &reference_type = static_cast<const TimeSeriesReferenceType &>(requested_item.type());
            fields.push_back({FieldMode::routed_reference, routed_count++,
                              &routed_time_series_reference_runtime_type(reference_type)});
        } else if (provided_item.is_reference() && !requested_item.is_reference()) {
            fields.push_back({FieldMode::dereferenced, dereferenced_count++, &requested_item});
        } else if (ReferenceProvider::representation_required(provided_item, requested_item)) {
            fields.push_back({FieldMode::adapted, adapted_count++, &requested_item});
        } else {
            fields.push_back({FieldMode::direct, 0, &provided_item});
        }
    }

    struct Entry {
        const TimeSeriesType *type;
        std::unique_ptr<FixedCollectionRepresentationRuntimeType> runtime_type;
    };
    static auto mutex = util::static_storage<std::mutex>{};
    static auto entries = util::static_storage<std::vector<Entry>>{};
    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = entries.get();
    const auto found = std::ranges::find_if(cache, [&](const auto &entry) {
        return entry.type == &requested.type() && entry.runtime_type->matches(fields);
    });
    if (found != cache.end()) {
        return *found->runtime_type;
    }
    auto runtime_type = std::make_unique<FixedCollectionRepresentationRuntimeType>(requested.type(), std::move(fields));
    auto &result = *runtime_type;
    cache.push_back({&requested.type(), std::move(runtime_type)});
    return result;
}

ReferenceRepresentation *create_representation(ReferenceProvider &provider, Reference base_reference,
                                               const TimeSeriesRuntimeType &, const TimeSeriesRuntimeType &requested,
                                               allocator &memory) {
    const auto &runtime_type = static_cast<const FixedCollectionRepresentationRuntimeType &>(requested);
    return memory.new_object<FixedCollectionRepresentation>(provider, base_reference, runtime_type);
}

const ReferenceRepresentationFactory factory{&representation_required, &representation_runtime_type,
                                             &create_representation};

} // namespace

const ReferenceRepresentationFactory *fixed_collection_reference_representation_factory() noexcept {
    return &factory;
}

} // namespace bg::details
