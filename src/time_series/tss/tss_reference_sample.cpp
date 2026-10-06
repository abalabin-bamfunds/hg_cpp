#include "tss_reference_sample.hpp"

#include <bg/util/index.hpp>

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

    std::size_t set_size(const void *value) const override;
    bool set_contains(const void *value, const ScalarData &key) const override;
    item_id_t find_set_item(const void *value, const ScalarData &key) const override;
    bool set_was_added(const TimeSeriesState &last_updated, const void *value, const ScalarData &key) const override;
    bool set_was_removed(const TimeSeriesState &last_updated, const void *value,
                         const ScalarData &key) const override;
    const void *set_key(const void *value, item_id_t id) const noexcept override;
    item_id_t set_next_item_id(const TimeSeriesState &last_updated, const void *value, Range range,
                               item_id_t start) const noexcept override;
    void construct_initial_value(void *) const noexcept override;
    void destroy_value(void *value, allocator &) const noexcept override;

  private:
    bool changed(const TimeSeriesState &last_updated, const SampledSet &sample, Range range,
                 const ScalarData &key) const;
};

const SampledSetRuntimeType &sampled_runtime_type(const TimeSeriesSetRuntimeType &shape);

} // namespace

struct SampledSet {
    SampledSet(const TimeSeriesSetRuntimeType &shape, ResolvedTimeSeries previous, ResolvedTimeSeries current,
               EvaluationTime evaluation_time, allocator &memory)
        : runtime_type{&sampled_runtime_type(shape)},
          previous_last_updated{previous.last_updated == nullptr ? initial_time_series_state : *previous.last_updated},
          current_last_updated{current.last_updated == nullptr ? initial_time_series_state : *current.last_updated},
          previous{previous}, current{current}, memory{&memory} {
        this->previous.last_updated = &previous_last_updated;
        this->current.last_updated = &current_last_updated;
        if (this->previous.valid()) {
            sampled_last_updated = previous_last_updated;
        }
        if (this->current.valid()) {
            sampled_last_updated.publish(evaluation_time);
        } else if (this->previous.valid()) {
            sampled_last_updated.invalidate(evaluation_time);
        }
    }

    void populate_changes() {
        if (this->previous.valid()) {
            const auto &previous_type = static_cast<const TimeSeriesSetRuntimeType &>(*this->previous.runtime_type);
            for (auto id = previous_type.set_next_item_id(previous_last_updated, this->previous.storage,
                                                          TimeSeriesSetRuntimeType::Range::current, 0);
                 id != util::occupancy_index::npos;
                 id = previous_type.set_next_item_id(previous_last_updated, this->previous.storage,
                                                      TimeSeriesSetRuntimeType::Range::current, id + 1)) {
                const auto key = ScalarData{previous_type.key_runtime_type(), previous_type.set_key(this->previous.storage, id)};
                if (!this->current.valid() ||
                    !static_cast<const TimeSeriesSetRuntimeType &>(*this->current.runtime_type)
                         .set_contains(this->current.storage, key)) {
                    removed.set(id, memory->memory_resource());
                }
            }
        }

        if (this->current.valid()) {
            const auto &current_type = static_cast<const TimeSeriesSetRuntimeType &>(*this->current.runtime_type);
            for (auto id = current_type.set_next_item_id(current_last_updated, this->current.storage,
                                                         TimeSeriesSetRuntimeType::Range::current, 0);
                 id != util::occupancy_index::npos;
                 id = current_type.set_next_item_id(current_last_updated, this->current.storage,
                                                    TimeSeriesSetRuntimeType::Range::current, id + 1)) {
                const auto key = ScalarData{current_type.key_runtime_type(), current_type.set_key(this->current.storage, id)};
                if (!this->previous.valid() ||
                    !static_cast<const TimeSeriesSetRuntimeType &>(*this->previous.runtime_type)
                         .set_contains(this->previous.storage, key)) {
                    added.set(id, memory->memory_resource());
                }
            }
        }
    }

    const SampledSetRuntimeType *runtime_type;
    TimeSeriesState previous_last_updated;
    TimeSeriesState current_last_updated;
    ResolvedTimeSeries previous;
    ResolvedTimeSeries current;
    TimeSeriesState sampled_last_updated;
    util::index<> added;
    util::index<> removed;
    allocator *memory;
};

namespace {

SampledSetRuntimeType::SampledSetRuntimeType(const TimeSeriesSetRuntimeType &shape)
    : TimeSeriesSetRuntimeType{static_cast<const TimeSeriesSetType &>(shape.type()), shape.key_runtime_type(),
                               sizeof(SampledSet), alignof(SampledSet), false, true} {}

const SampledSetRuntimeType &sampled_runtime_type(const TimeSeriesSetRuntimeType &shape) {
    struct Entry {
        const TimeSeriesSetRuntimeType *shape;
        std::unique_ptr<SampledSetRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find(entries, &shape, &Entry::shape);
    if (existing != entries.end()) {
        return *existing->runtime_type;
    }

    auto runtime_type = std::make_unique<SampledSetRuntimeType>(shape);
    const auto &result = *runtime_type;
    entries.push_back({&shape, std::move(runtime_type)});
    return result;
}

const SampledSet &sample(const void *value) noexcept { return *static_cast<const SampledSet *>(value); }

item_id_t next_position(const util::index<> &positions, item_id_t start, bool previous) noexcept {
    const auto position = positions.first_set(previous ? start & ~previous_item_flag : start);
    if (position == positions.size()) {
        return util::occupancy_index::npos;
    }
    return previous ? (position | previous_item_flag) : position;
}

bool SampledSetRuntimeType::changed(const TimeSeriesState &last_updated, const SampledSet &sample, Range range,
                                    const ScalarData &key) const {
    if (last_updated.time() != sample.sampled_last_updated.time()) {
        return false;
    }

    const auto previous = range == Range::removed;
    const auto &time_series = previous ? sample.previous : sample.current;
    if (!time_series.valid()) {
        return false;
    }
    const auto &runtime_type = static_cast<const TimeSeriesSetRuntimeType &>(*time_series.runtime_type);
    const auto position = runtime_type.find_set_item(time_series.storage, key);
    return (previous ? sample.removed : sample.added).contains(position);
}

std::size_t SampledSetRuntimeType::set_size(const void *value) const {
    const auto &current = sample(value).current;
    return current.valid() ? static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                 .set_size(current.storage)
                           : 0;
}

bool SampledSetRuntimeType::set_contains(const void *value, const ScalarData &key) const {
    const auto &current = sample(value).current;
    return current.valid() && static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                  .set_contains(current.storage, key);
}

item_id_t SampledSetRuntimeType::find_set_item(const void *value, const ScalarData &key) const {
    const auto &current = sample(value).current;
    return current.valid() ? static_cast<const TimeSeriesSetRuntimeType &>(*current.runtime_type)
                                 .find_set_item(current.storage, key)
                           : util::occupancy_index::npos;
}

bool SampledSetRuntimeType::set_was_added(const TimeSeriesState &last_updated, const void *value,
                                          const ScalarData &key) const {
    return changed(last_updated, sample(value), Range::added, key);
}

bool SampledSetRuntimeType::set_was_removed(const TimeSeriesState &last_updated, const void *value,
                                            const ScalarData &key) const {
    return changed(last_updated, sample(value), Range::removed, key);
}

const void *SampledSetRuntimeType::set_key(const void *value, item_id_t id) const noexcept {
    const auto &sampled = sample(value);
    const auto from_previous = (id & previous_item_flag) != 0;
    const auto &time_series = from_previous ? sampled.previous : sampled.current;
    const auto position = id & ~previous_item_flag;
    return static_cast<const TimeSeriesSetRuntimeType &>(*time_series.runtime_type)
        .set_key(time_series.storage, position);
}

item_id_t SampledSetRuntimeType::set_next_item_id(const TimeSeriesState &last_updated, const void *value,
                                                  Range range, item_id_t start) const noexcept {
    const auto &sampled = sample(value);
    if (range == Range::current) {
        if (!sampled.current.valid()) {
            return util::occupancy_index::npos;
        }
        return static_cast<const TimeSeriesSetRuntimeType &>(*sampled.current.runtime_type)
            .set_next_item_id(*sampled.current.last_updated, sampled.current.storage, Range::current, start);
    }
    if (last_updated.time() != sampled.sampled_last_updated.time() ||
        (range == Range::added && !last_updated.valid()) ||
        (range == Range::added && !sampled.current.valid()) ||
        (range == Range::removed && !sampled.current.valid() && !sampled.previous.valid())) {
        return util::occupancy_index::npos;
    }
    const auto &positions = range == Range::added ? sampled.added : sampled.removed;
    return next_position(positions, start, range == Range::removed);
}

void SampledSetRuntimeType::construct_initial_value(void *) const noexcept { std::terminate(); }

void SampledSetRuntimeType::destroy_value(void *value, allocator &) const noexcept {
    destroy_set_sample(static_cast<SampledSet *>(value));
}

} // namespace

SampledSet *make_set_sample(ResolvedTimeSeries previous, ResolvedTimeSeries current, EvaluationTime evaluation_time,
                            allocator &memory) {
    if (previous.runtime_type == nullptr && current.runtime_type == nullptr) {
        return nullptr;
    }

    const auto &shape = static_cast<const TimeSeriesSetRuntimeType &>(
        *(current.runtime_type != nullptr ? current.runtime_type : previous.runtime_type));
    auto *sample = memory.new_object<SampledSet>(shape, previous, current, evaluation_time, memory);
    auto cleanup = util::scope_exit{[&]() noexcept { destroy_set_sample(sample); }};
    sample->populate_changes();
    cleanup.release();
    return sample;
}

void destroy_set_sample(SampledSet *sample) noexcept {
    auto *memory = sample->memory;
    sample->added.release(memory->memory_resource());
    sample->removed.release(memory->memory_resource());
    memory->delete_object(sample);
}

TimeSeriesRuntimeType::ConstItem set_sample_item(const SampledSet &sample) noexcept {
    return {*sample.runtime_type, sample.sampled_last_updated, &sample};
}

ResolvedTimeSeries set_sample_previous(const SampledSet &sample) noexcept { return sample.previous; }

} // namespace bg::details
