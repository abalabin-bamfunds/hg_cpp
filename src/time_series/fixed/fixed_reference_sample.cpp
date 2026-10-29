#include "fixed_reference_sample.hpp"

#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/static_storage.hpp>

#include <cassert>
#include <exception>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {
namespace {

class SampledFixedCollectionRuntimeType final : public TimeSeriesRuntimeType {
  public:
    explicit SampledFixedCollectionRuntimeType(const TimeSeriesRuntimeType &shape)
        : TimeSeriesRuntimeType{shape.type()}, shape_{&shape} {}

    static const SampledFixedCollectionRuntimeType &for_shape(const TimeSeriesRuntimeType &shape) {
        struct Entry {
            const TimeSeriesRuntimeType *shape;
            std::unique_ptr<SampledFixedCollectionRuntimeType> runtime_type;
        };

        static auto mutex = util::static_storage<std::mutex>{};
        static auto entries = util::static_storage<std::vector<Entry>>{};

        const auto lock = std::lock_guard{mutex.get()};
        auto &cache = entries.get();
        const auto found = std::ranges::find(cache, &shape, &Entry::shape);
        if (found != cache.end()) {
            return *found->runtime_type;
        }

        auto runtime_type = std::make_unique<SampledFixedCollectionRuntimeType>(shape);
        const auto &result = *runtime_type;
        cache.push_back({&shape, std::move(runtime_type)});
        return result;
    }

    ItemLayout item_layout(item_id_t) const override {
        throw std::logic_error{"a sampled fixed collection has no packed item layout"};
    }

    const TimeSeriesRuntimeType &item_runtime_type(item_id_t id) const override {
        return shape_->item_runtime_type(id);
    }

    std::size_t item_count(const void *) const override {
        if (const auto *list = type_cast<TimeSeriesListType>(&type())) {
            return list->item_count();
        }
        return static_cast<const TimeSeriesBundleType &>(type()).item_count();
    }

    ConstItem item(const void *value, item_id_t id) const override {
        return static_cast<const SampledFixedCollection *>(value)->item(id);
    }

    MutableItem item(void *, item_id_t) const override {
        throw std::logic_error{"a sampled fixed collection is immutable"};
    }

    void for_each_item_id(const void *, void *context, ItemIdFunction function) const override {
        for (auto id = item_id_t{}; id < item_count(nullptr); ++id) {
            function(context, id);
        }
    }

    bool all_valid(const TimeSeriesState &last_updated, const void *value) const noexcept override {
        return all_valid(last_updated, value, type());
    }

    bool all_valid(const TimeSeriesState &last_updated, const void *value,
                   const TimeSeriesType &declared_type) const noexcept override {
        if (!last_updated.valid() || value == nullptr) {
            return false;
        }

        const auto &sample = *static_cast<const SampledFixedCollection *>(value);
        return sample.current.runtime_type != nullptr && sample.current.storage != nullptr &&
               sample.current.runtime_type->all_valid(sample.current.last_updated, sample.current.storage,
                                                      declared_type);
    }

    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t) const override {
        if (!last_updated.valid() || value == nullptr) {
            return false;
        }
        const auto &sample = *static_cast<const SampledFixedCollection *>(value);
        return last_updated.time() == sample.sampled_last_updated.time();
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        if (!last_updated.valid() || value == nullptr ||
            last_updated.time() != static_cast<const SampledFixedCollection *>(value)->sampled_last_updated.time()) {
            return;
        }

        for_each_item_id(value, context, function);
    }

    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const SampledFixedCollection *>(value)->current.valid();
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        if (last_updated.time() != evaluation_time) {
            return;
        }

        for (auto id = item_id_t{}; id < item_count(nullptr); ++id) {
            const auto item = static_cast<const SampledFixedCollection *>(value)->item(id);
            function(context, {id, &item.runtime_type, &item.last_updated, item.value});
        }
    }

    void construct_initial_value(void *) const noexcept override { std::terminate(); }
    void destroy_value(void *, allocator &) const noexcept override { std::terminate(); }

  private:
    const TimeSeriesRuntimeType *shape_;
};

} // namespace

SampledFixedCollection::SampledFixedCollection(const TimeSeriesRuntimeType &shape, ResolvedTimeSeries previous,
                                               ResolvedTimeSeries current, EvaluationTime evaluation_time,
                                               allocator &memory)
    : runtime_type{&SampledFixedCollectionRuntimeType::for_shape(shape)}, previous{previous}, current{current},
      sampled_last_updated{TimeSeriesState::from_validity_transition(
          evaluation_time, previous.last_updated.valid_before(evaluation_time), current.last_updated.valid())},
      memory{memory} {}

SampledFixedCollection::~SampledFixedCollection() {
    child_samples.release_with(child_samples_positions, memory.memory_resource(),
                               [](SampledReferenceChange *sample) noexcept { sample->release(); });
    child_samples_positions.release(memory.memory_resource());
}

SampledFixedCollection *SampledFixedCollection::create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                                       EvaluationTime evaluation_time, allocator &memory) {
    const auto *shape = current.runtime_type != nullptr ? current.runtime_type : previous.runtime_type;
    assert(shape != nullptr);
    return memory.new_object<SampledFixedCollection>(*shape, previous, current, evaluation_time, memory);
}

void SampledFixedCollection::release() noexcept {
    if (--reference_count == 0) {
        memory.delete_object(this);
    }
}

TimeSeriesRuntimeType::ConstItem SampledFixedCollection::input() const noexcept {
    return {*runtime_type, sampled_last_updated, this};
}

TimeSeriesRuntimeType::ConstItem SampledFixedCollection::item(item_id_t id) const {
    const auto &shape = *runtime_type;
    if (const auto *sample = child_samples.get(id, child_samples_positions)) {
        return sample->input();
    }

    const auto &expected_type = shape.item_runtime_type(id);
    const auto previous_item = previous.runtime_type != nullptr && previous.storage != nullptr
                                   ? previous.runtime_type->item(previous.storage, id)
                                   : TimeSeriesRuntimeType::ConstItem{expected_type, previous.last_updated, nullptr};
    const auto current_item = current.runtime_type != nullptr && current.storage != nullptr
                                  ? current.runtime_type->item(current.storage, id)
                                  : TimeSeriesRuntimeType::ConstItem{expected_type, current.last_updated, nullptr};
    const auto previous_child = ResolvedTimeSeries{previous_item.last_updated, previous_item.value,
                                                   &previous_item.runtime_type};
    const auto current_child = ResolvedTimeSeries{current_item.last_updated, current_item.value,
                                                  &current_item.runtime_type};

    auto sample_memory = memory;
    auto nested_sample = SampledReferenceChange::create(previous_child, current_child, sampled_last_updated.time(),
                                                        sample_memory);
    if (nested_sample) {
        auto release_nested_sample = util::scope_exit{[&]() noexcept { nested_sample.release(); }};
        const auto inserted_position = child_samples_positions.set(id, memory.memory_resource());
        assert(inserted_position);
        auto reset_position = util::scope_exit{[&]() noexcept { child_samples_positions.reset(id); }};
        child_samples.emplace_at(id, memory.memory_resource(), nested_sample);
        reset_position.release();
        release_nested_sample.release();
        return nested_sample.input();
    }

    const auto was_valid = previous_child.last_updated.valid_before(sampled_last_updated.time());
    const auto is_valid = current_child.last_updated.valid();
    auto last_updated = TimeSeriesState::from_validity_transition(sampled_last_updated.time(), was_valid, is_valid);
    return {*current_child.runtime_type, last_updated, current_child.storage};
}

} // namespace bg::details
