#include "tss_reference_sample.hpp"

#include <bg/util/index.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <exception>
#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {
namespace {

constexpr auto previous_item_flag = item_id_t{1} << 31;

class SampledSetRuntimeType final : public TimeSeriesSetRuntimeType {
  public:
    explicit SampledSetRuntimeType(const TimeSeriesSetRuntimeType &shape);
    static const SampledSetRuntimeType &for_shape(const TimeSeriesSetRuntimeType &shape);

    std::size_t set_size(const void *value) const override;
    bool set_contains(const void *value, const ScalarData &key) const override;
    item_id_t find_set_item(const void *value, const ScalarData &key) const override;
    item_id_t find_set_key(const void *value, const ScalarData &key) const override;
    bool set_was_added(const TimeSeriesState &last_updated, const void *value, const ScalarData &key) const override;
    bool set_was_removed(const TimeSeriesState &last_updated, const void *value,
                         const ScalarData &key) const override;
    RetainedSetKey retain_key(void *value, const ScalarData &key, allocator &memory) const override;
    void release_key(void *, item_id_t, allocator &) const noexcept override;
    const void *set_key(const void *value, item_id_t id) const noexcept override;
    item_id_t set_next_item_id(const TimeSeriesState &last_updated, const void *value, Range range,
                               item_id_t start) const override;
    bool has_item_id_remapping(const void *value) const override;
    void for_each_item_id_remapping(const void *value, void *context,
                                    ItemIdRemappingFunction function) const override;
    void construct_initial_value(void *) const noexcept override;
    void destroy_value(void *value, allocator &) const noexcept override;

  private:
    bool changed(const TimeSeriesState &last_updated, const SampledSet &sample, Range range,
                 const ScalarData &key) const;
};

} // namespace

SampledSet::SampledSet(const TimeSeriesSetRuntimeType &shape, ResolvedTimeSeries previous, ResolvedTimeSeries current,
                       EvaluationTime evaluation_time, allocator &memory)
    : runtime_type{&SampledSetRuntimeType::for_shape(shape)},
      previous{previous}, current{current}, item_id_remappings{&memory.memory_resource()}, memory{memory} {
    sampled_last_updated = TimeSeriesState::from_validity_transition(
        evaluation_time, previous.last_updated.valid_before(evaluation_time), current.last_updated.valid());
}

SampledSet::~SampledSet() {
    added.release(memory);
    removed.release(memory);
}

bool SampledSet::previous_contains(const ScalarData &key) const {
    if (!previous.valid()) {
        return false;
    }

    const auto &previous_type = static_cast<const TimeSeriesSetRuntimeType &>(*previous.runtime_type);
    const auto changed_this_cycle = previous.last_updated.time() == sampled_last_updated.time();
    if (previous_type.set_contains(previous.storage, key)) {
        return !changed_this_cycle || !previous_type.set_was_added(previous.last_updated, previous.storage, key);
    }
    return changed_this_cycle && previous_type.set_was_removed(previous.last_updated, previous.storage, key);
}

void SampledSet::populate_changes() const {
    if (changes_populated) {
        return;
    }

    const auto original_remapping_count = item_id_remappings.size();
    auto committed = false;
    auto rollback = util::scope_exit{[&]() noexcept {
        if (!committed) {
            item_id_remappings.resize(original_remapping_count);
        }
    }};

    const auto previous_changed_this_cycle =
        previous.valid() && previous.last_updated.time() == sampled_last_updated.time();
    const auto *previous_type = previous.valid()
                                    ? &static_cast<const TimeSeriesSetRuntimeType &>(*previous.runtime_type)
                                    : nullptr;
    const auto *current_type = current.valid()
                                   ? &static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                   : nullptr;

    const auto record_previous_item = [&](item_id_t id) {
        const auto key = ScalarData{previous_type->key_runtime_type(), previous_type->set_key(previous.storage, id)};
        if (previous_changed_this_cycle &&
            previous_type->set_was_added(previous.last_updated, previous.storage, key)) {
            return;
        }

        const auto current_id = current_type == nullptr ? util::occupancy_index::npos
                                                         : current_type->find_set_item(current.storage, key);
        if (current_id == util::occupancy_index::npos) {
            removed.set(id, memory);
        } else if (current_id != id) {
            item_id_remappings.push_back({id, current_id});
        }
    };

    if (previous_type != nullptr) {
        for (auto id = previous_type->set_next_item_id(previous.last_updated, previous.storage,
                                                       TimeSeriesSetRuntimeType::Range::current, 0);
             id != util::occupancy_index::npos;
             id = previous_type->set_next_item_id(previous.last_updated, previous.storage,
                                                  TimeSeriesSetRuntimeType::Range::current, id + 1)) {
            record_previous_item(id);
        }

        if (previous_changed_this_cycle) {
            for (auto id = previous_type->set_next_item_id(previous.last_updated, previous.storage,
                                                           TimeSeriesSetRuntimeType::Range::removed, 0);
                 id != util::occupancy_index::npos;
                 id = previous_type->set_next_item_id(previous.last_updated, previous.storage,
                                                      TimeSeriesSetRuntimeType::Range::removed, id + 1)) {
                record_previous_item(id);
            }
        }
    }

    if (current_type != nullptr) {
        for (auto id = current_type->set_next_item_id(current.last_updated, current.storage,
                                                      TimeSeriesSetRuntimeType::Range::current, 0);
             id != util::occupancy_index::npos;
             id = current_type->set_next_item_id(current.last_updated, current.storage,
                                                 TimeSeriesSetRuntimeType::Range::current, id + 1)) {
            const auto key = ScalarData{current_type->key_runtime_type(), current_type->set_key(current.storage, id)};
            if (!previous_contains(key)) {
                added.set(id, memory);
            }
        }
    }
    changes_populated = true;
    committed = true;
    rollback.release();
}

namespace {

SampledSetRuntimeType::SampledSetRuntimeType(const TimeSeriesSetRuntimeType &shape)
    : TimeSeriesSetRuntimeType{static_cast<const TimeSeriesSetType &>(shape.type()), shape.key_runtime_type(),
                               sizeof(SampledSet), alignof(SampledSet), false, true} {}

const SampledSetRuntimeType &SampledSetRuntimeType::for_shape(const TimeSeriesSetRuntimeType &shape) {
    struct Entry {
        const TimeSeriesSetRuntimeType *shape;
        std::unique_ptr<SampledSetRuntimeType> runtime_type;
    };

    static auto mutex = util::static_storage<std::mutex>{};
    static auto entries = util::static_storage<std::vector<Entry>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = entries.get();
    const auto existing = std::ranges::find(cache, &shape, &Entry::shape);
    if (existing != cache.end()) {
        return *existing->runtime_type;
    }

    auto runtime_type = std::make_unique<SampledSetRuntimeType>(shape);
    const auto &result = *runtime_type;
    cache.push_back({&shape, std::move(runtime_type)});
    return result;
}

bool SampledSetRuntimeType::changed(const TimeSeriesState &last_updated, const SampledSet &sample, Range range,
                                    const ScalarData &key) const {
    if (last_updated.time() != sample.sampled_last_updated.time()) {
        return false;
    }

    const auto previous_contains = sample.previous_contains(key);
    const auto current_contains = sample.current.valid() &&
                                  static_cast<const TimeSeriesSetRuntimeType &>(*sample.current.runtime_type)
                                      .set_contains(sample.current.storage, key);
    return range == Range::added ? current_contains && !previous_contains : previous_contains && !current_contains;
}

std::size_t SampledSetRuntimeType::set_size(const void *value) const {
    const auto &current = static_cast<const SampledSet *>(value)->current;
    return current.valid() ? static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                 .set_size(current.storage)
                           : 0;
}

bool SampledSetRuntimeType::set_contains(const void *value, const ScalarData &key) const {
    const auto &current = static_cast<const SampledSet *>(value)->current;
    return current.valid() && static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                  .set_contains(current.storage, key);
}

item_id_t SampledSetRuntimeType::find_set_item(const void *value, const ScalarData &key) const {
    const auto &current = static_cast<const SampledSet *>(value)->current;
    return current.valid() ? static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                 .find_set_item(current.storage, key)
                           : util::occupancy_index::npos;
}

item_id_t SampledSetRuntimeType::find_set_key(const void *value, const ScalarData &key) const {
    const auto &current = static_cast<const SampledSet *>(value)->current;
    if (current.runtime_type == nullptr || current.storage == nullptr) {
        return util::occupancy_index::npos;
    }
    return static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type).find_set_key(current.storage, key);
}

bool SampledSetRuntimeType::set_was_added(const TimeSeriesState &last_updated, const void *value,
                                          const ScalarData &key) const {
    return changed(last_updated, *static_cast<const SampledSet *>(value), Range::added, key);
}

bool SampledSetRuntimeType::set_was_removed(const TimeSeriesState &last_updated, const void *value,
                                            const ScalarData &key) const {
    return changed(last_updated, *static_cast<const SampledSet *>(value), Range::removed, key);
}

RetainedSetKey SampledSetRuntimeType::retain_key(void *value, const ScalarData &key, allocator &memory) const {
    if (value == nullptr) {
        return {};
    }

    const auto &current = static_cast<SampledSet *>(value)->current;
    if (current.runtime_type == nullptr || current.storage == nullptr) {
        return {};
    }

    return static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
        .retain_key(const_cast<void *>(current.storage), key, memory);
}

void SampledSetRuntimeType::release_key(void *, item_id_t, allocator &) const noexcept {
    assert(false && "sampled set key retention must be released through its retained runtime type");
}

const void *SampledSetRuntimeType::set_key(const void *value, item_id_t id) const noexcept {
    const auto &sampled = *static_cast<const SampledSet *>(value);
    const auto from_previous = (id & previous_item_flag) != 0;
    const auto &time_series = from_previous ? sampled.previous : sampled.current;
    const auto position = id & ~previous_item_flag;
    return static_cast<const TimeSeriesSetRuntimeType &>(*time_series.runtime_type)
        .set_key(time_series.storage, position);
}

item_id_t SampledSetRuntimeType::set_next_item_id(const TimeSeriesState &last_updated, const void *value,
                                                  Range range, item_id_t start) const {
    const auto &sampled = *static_cast<const SampledSet *>(value);
    if (range == Range::current) {
        if (!sampled.current.valid()) {
            return util::occupancy_index::npos;
        }
        return static_cast<const TimeSeriesSetRuntimeType &>(*sampled.current.runtime_type)
            .set_next_item_id(sampled.current.last_updated, sampled.current.storage, Range::current, start);
    }
    sampled.populate_changes();
    if (last_updated.time() != sampled.sampled_last_updated.time() ||
        (range == Range::added && !last_updated.valid()) ||
        (range == Range::added && !sampled.current.valid()) ||
        (range == Range::removed && !sampled.current.valid() && !sampled.previous.valid())) {
        return util::occupancy_index::npos;
    }
    const auto &positions = range == Range::added ? sampled.added : sampled.removed;
    const auto previous = range == Range::removed;
    const auto position = positions.first_set(previous ? start & ~previous_item_flag : start);
    if (position == positions.size()) {
        return util::occupancy_index::npos;
    }
    return previous ? (position | previous_item_flag) : position;
}

bool SampledSetRuntimeType::has_item_id_remapping(const void *value) const {
    if (value == nullptr) {
        return false;
    }
    const auto &sample = *static_cast<const SampledSet *>(value);
    sample.populate_changes();
    return !sample.item_id_remappings.empty();
}

void SampledSetRuntimeType::for_each_item_id_remapping(const void *value, void *context,
                                                       ItemIdRemappingFunction function) const {
    if (value == nullptr) {
        return;
    }
    const auto &sample = *static_cast<const SampledSet *>(value);
    sample.populate_changes();
    for (const auto remapping : sample.item_id_remappings) {
        function(context, remapping.old_id, remapping.new_id);
    }
}

void SampledSetRuntimeType::construct_initial_value(void *) const noexcept { std::terminate(); }

void SampledSetRuntimeType::destroy_value(void *value, allocator &) const noexcept {
    static_cast<SampledSet *>(value)->release();
}

} // namespace

SampledSet *SampledSet::create(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                               EvaluationTime evaluation_time, allocator &memory) {
    if (previous.runtime_type == nullptr && current.runtime_type == nullptr) {
        return nullptr;
    }

    const auto &shape = static_cast<const TimeSeriesSetRuntimeType &>(
        *(current.runtime_type != nullptr ? current.runtime_type : previous.runtime_type));
    return memory.new_object<SampledSet>(shape, previous, current, evaluation_time, memory);
}

void SampledSet::release() noexcept {
    if (--reference_count == 0) {
        memory.delete_object(this);
    }
}

TimeSeriesRuntimeType::ConstItem SampledSet::input() const noexcept {
    return {*runtime_type, sampled_last_updated, this};
}

} // namespace bg::details
