#include "tsd_reference_sample.hpp"

#include <bg/allocator.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {

class SampledDictionaryRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    explicit SampledDictionaryRuntimeType(const TimeSeriesDictionaryRuntimeType &shape)
        : TimeSeriesDictionaryRuntimeType{static_cast<const TimeSeriesDictionaryType &>(shape.type()),
                                          shape.key_runtime_type(), shape.item_runtime_type()} {}

    DictionaryKeyStorage retention_storage(void *value) const noexcept override;
    RetainedDictionaryKey retain_key(void *, const ScalarData &, allocator &) const override;
    void release_key(void *, item_id_t, allocator &) const noexcept override;
    item_id_t find_key(const void *value, const ScalarData &key) const override;
    bool key_live(const void *value, item_id_t id) const noexcept override;
    item_id_t find_item(const void *value, const ScalarData &key) const override;
    const void *key(const void *value, item_id_t id) const noexcept override;
    void for_each_added(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    void for_each_removed(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    void for_each_modified(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    bool item_modified(const EvaluationTime &, const void *, item_id_t id) const noexcept override;
    std::size_t item_count(const void *value) const override;
    ConstItem item(const void *value, item_id_t id) const override;
    MutableItem item(void *, item_id_t) const override;
    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override;
    bool item_updated(const EvaluationTime &, const void *, item_id_t id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &, const void *, void *, ItemIdFunction) const override;
    bool collection_valid(const void *value) const noexcept override;
    void for_each_modified_item(const EvaluationTime &, const void *, EvaluationTime, void *,
                                ModifiedItemFunction) const override;
};

const SampledDictionaryRuntimeType &sampled_runtime_type(const TimeSeriesDictionaryRuntimeType &shape);

constexpr auto previous_item_flag = item_id_t{1} << 31;

struct SampledDictionary {
    struct Child {
        item_id_t id;
        SampledDictionary *sample;
    };

    SampledDictionary(const TimeSeriesDictionaryRuntimeType &shape, ResolvedTimeSeries previous,
                      ResolvedTimeSeries current, EvaluationTime evaluation_time, allocator &memory)
        : runtime_type{&sampled_runtime_type(shape)},
          previous_last_updated{previous.last_updated == nullptr ? MIN_DT : *previous.last_updated},
          current_last_updated{current.last_updated == nullptr ? MIN_DT : *current.last_updated}, previous{previous},
          current{current}, evaluation_time{evaluation_time}, children{&memory.memory_resource()}, memory{&memory} {
        this->previous.last_updated = &previous_last_updated;
        this->current.last_updated = &current_last_updated;
    }

    const SampledDictionaryRuntimeType *runtime_type;
    EvaluationTime previous_last_updated;
    EvaluationTime current_last_updated;
    ResolvedTimeSeries previous;
    ResolvedTimeSeries current;
    EvaluationTime evaluation_time;
    util::index<> added;
    util::index<> removed;
    util::index<> modified;
    std::pmr::vector<Child> children;
    allocator *memory;
};

const TimeSeriesDictionaryRuntimeType *dictionary_runtime_type(ResolvedTimeSeries time_series) noexcept {
    return time_series.runtime_type == nullptr
               ? nullptr
               : dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(time_series.runtime_type);
}

bool same_item(const TimeSeriesRuntimeType::ConstItem &left, const TimeSeriesRuntimeType::ConstItem &right) noexcept {
    if (dynamic_cast<const TimeSeriesReferenceType *>(&left.runtime_type.type()) != nullptr &&
        dynamic_cast<const TimeSeriesReferenceType *>(&right.runtime_type.type()) != nullptr && left.value != nullptr &&
        right.value != nullptr) {
        return left.runtime_type.reference(left.value) == right.runtime_type.reference(right.value);
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

SampledDictionary *make_dictionary_sample(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                          EvaluationTime evaluation_time, allocator &memory) {
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
    auto *sample = memory.new_object<SampledDictionary>(shape, previous, current, evaluation_time, memory);
    auto destroy = util::scope_exit{[&]() noexcept { destroy_dictionary_sample(sample); }};

    if (current.valid()) {
        struct CurrentCallback {
            SampledDictionary *sample;
            const TimeSeriesDictionaryRuntimeType *current_type;
            const TimeSeriesDictionaryRuntimeType *previous_type;
        } callback{sample, current_type, previous_type};
        current_type->for_each_item_id(current.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<CurrentCallback *>(value);
            auto &sample = *callback.sample;
            const auto key = ScalarData{callback.current_type->key_runtime_type(),
                                        callback.current_type->key(sample.current.storage, id)};
            const auto previous_id = sample.previous.valid()
                                         ? callback.previous_type->find_item(sample.previous.storage, key)
                                         : util::occupancy_index::npos;
            if (previous_id == util::occupancy_index::npos) {
                sample.added.set(id, *sample.memory);
                return;
            }

            const auto current_item = callback.current_type->item(sample.current.storage, id);
            const auto previous_item = callback.previous_type->item(sample.previous.storage, previous_id);
            if (same_item(current_item, previous_item)) {
                if (callback.current_type->item_modified(*sample.current.last_updated, sample.current.storage, id)) {
                    sample.modified.set(id, *sample.memory);
                }
                return;
            }

            sample.modified.set(id, *sample.memory);
            const auto previous_time_series =
                ResolvedTimeSeries{&previous_item.last_updated, previous_item.value, &previous_item.runtime_type};
            const auto current_time_series =
                ResolvedTimeSeries{&current_item.last_updated, current_item.value, &current_item.runtime_type};
            if (auto *child = make_dictionary_sample(previous_time_series, current_time_series, sample.evaluation_time,
                                                     *sample.memory)) {
                sample.children.push_back({id, child});
            }
        });
    }

    if (previous.valid()) {
        struct PreviousCallback {
            SampledDictionary *sample;
            const TimeSeriesDictionaryRuntimeType *current_type;
            const TimeSeriesDictionaryRuntimeType *previous_type;
        } callback{sample, current_type, previous_type};
        previous_type->for_each_item_id(previous.storage, &callback, [](void *value, item_id_t id) {
            auto &callback = *static_cast<PreviousCallback *>(value);
            auto &sample = *callback.sample;
            const auto key = ScalarData{callback.previous_type->key_runtime_type(),
                                        callback.previous_type->key(sample.previous.storage, id)};
            if (sample.current.valid() &&
                callback.current_type->find_item(sample.current.storage, key) != util::occupancy_index::npos) {
                return;
            }
            sample.removed.set(id, *sample.memory);
        });
    }

    destroy.release();
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
    memory.delete_object(sample);
}

item_id_t SampledDictionaryRuntimeType::find_item(const void *value, const ScalarData &key) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (!sample.current.valid()) {
        return util::occupancy_index::npos;
    }
    return dictionary_runtime_type(sample.current)->find_item(sample.current.storage, key);
}

RetainedDictionaryKey SampledDictionaryRuntimeType::retain_key(void *value, const ScalarData &key,
                                                               allocator &memory) const {
    auto &sample = *static_cast<SampledDictionary *>(value);
    auto *runtime_type = dictionary_runtime_type(sample.current);
    if (runtime_type == nullptr || sample.current.storage == nullptr) {
        return {};
    }

    return runtime_type->retain_key(const_cast<void *>(sample.current.storage), key, memory);
}

DictionaryKeyStorage SampledDictionaryRuntimeType::retention_storage(void *value) const noexcept {
    auto &sample = *static_cast<SampledDictionary *>(value);
    auto *runtime_type = dictionary_runtime_type(sample.current);
    if (runtime_type == nullptr || sample.current.storage == nullptr) {
        return {};
    }

    return runtime_type->retention_storage(const_cast<void *>(sample.current.storage));
}

void SampledDictionaryRuntimeType::release_key(void *, item_id_t, allocator &) const noexcept {
    assert(false && "sampled dictionary key retention must be released through its retained runtime type");
}

item_id_t SampledDictionaryRuntimeType::find_key(const void *value, const ScalarData &key) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    const auto *runtime_type = dictionary_runtime_type(sample.current);
    if (runtime_type == nullptr || sample.current.storage == nullptr) {
        return util::occupancy_index::npos;
    }

    return runtime_type->find_key(sample.current.storage, key);
}

bool SampledDictionaryRuntimeType::key_live(const void *value, item_id_t id) const noexcept {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    const auto *runtime_type = dictionary_runtime_type(sample.current);
    return runtime_type != nullptr && sample.current.storage != nullptr &&
           runtime_type->key_live(sample.current.storage, id);
}

const void *SampledDictionaryRuntimeType::key(const void *value, item_id_t id) const noexcept {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if ((id & previous_item_flag) != 0) {
        return dictionary_runtime_type(sample.previous)->key(sample.previous.storage, id & ~previous_item_flag);
    }
    return dictionary_runtime_type(sample.current)->key(sample.current.storage, id);
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
                                                 item_id_t id) const noexcept {
    return static_cast<const SampledDictionary *>(value)->modified.contains(id);
}

std::size_t SampledDictionaryRuntimeType::item_count(const void *value) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    return sample.current.valid() ? dictionary_runtime_type(sample.current)->item_count(sample.current.storage) : 0;
}

TimeSeriesRuntimeType::ConstItem SampledDictionaryRuntimeType::item(const void *value, item_id_t id) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (id == key_set_item_id) {
        const auto time_series = sample.current.valid() ? sample.current : sample.previous;
        auto item = dictionary_runtime_type(time_series)->item(time_series.storage, id);
        return {item.runtime_type, sample.evaluation_time, item.value};
    }
    if ((id & previous_item_flag) != 0) {
        const auto item =
            dictionary_runtime_type(sample.previous)->item(sample.previous.storage, id & ~previous_item_flag);
        return {item.runtime_type, MIN_DT, item.value};
    }

    const auto child = std::ranges::lower_bound(sample.children, id, {}, &SampledDictionary::Child::id);
    if (child != sample.children.end() && child->id == id) {
        const auto &visible_time = child->sample->current.valid() ? child->sample->evaluation_time : MIN_DT;
        return {*child->sample->runtime_type, visible_time, child->sample};
    }

    const auto item = dictionary_runtime_type(sample.current)->item(sample.current.storage, id);
    const auto &last_updated =
        sample.added.contains(id) || sample.modified.contains(id) ? sample.evaluation_time : item.last_updated;
    return {item.runtime_type, last_updated, item.value};
}

TimeSeriesRuntimeType::MutableItem SampledDictionaryRuntimeType::item(void *, item_id_t) const {
    throw std::logic_error{"a sampled dictionary is immutable"};
}

void SampledDictionaryRuntimeType::for_each_item_id(const void *value, void *context, ItemIdFunction function) const {
    const auto &sample = *static_cast<const SampledDictionary *>(value);
    if (sample.current.valid()) {
        dictionary_runtime_type(sample.current)->for_each_item_id(sample.current.storage, context, function);
    }
}

bool SampledDictionaryRuntimeType::item_updated(const EvaluationTime &, const void *value,
                                                item_id_t id) const noexcept {
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
    return static_cast<const SampledDictionary *>(value)->current.valid();
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

TimeSeriesRuntimeType::ConstItem dictionary_sample_item(const SampledDictionary &sample) noexcept {
    const auto &visible_time = sample.current.valid() ? sample.evaluation_time : MIN_DT;
    return {*sample.runtime_type, visible_time, &sample};
}

ResolvedTimeSeries dictionary_sample_previous(const SampledDictionary &sample) noexcept {
    return sample.previous;
}

} // namespace bg::details
