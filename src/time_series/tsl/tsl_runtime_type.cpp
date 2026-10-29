#include <bg/time_series/tsl/tsl_runtime_type.hpp>

#include "../fixed/fixed_reference_representation.hpp"
#include <bg/util/static_storage.hpp>
#include <bg/time_series/tsl/tsl_free_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

const ReferenceRepresentationFactory *FreeFixedTimeSeriesListRuntimeType::reference_representation_factory()
    const noexcept {
    return fixed_collection_reference_representation_factory();
}

const ReferenceRepresentationFactory *FixedTimeSeriesListRuntimeType::reference_representation_factory()
    const noexcept {
    return fixed_collection_reference_representation_factory();
}

void FixedTimeSeriesListRuntimeType::copy_tick(const TimeSeriesInputData &input,
                                              OutputState &output) const {
    if (input.valid()) {
        const auto *source_type = type_cast<TimeSeriesListType>(&input.runtime_type().type());
        if (source_type == nullptr || source_type->item_count() != item_count(output.data())) {
            throw std::invalid_argument{"time series list copy requires a matching item count"};
        }
    }

    auto source = In<TSL<TS_T>>{input};
    auto target = output_factory::make<Out<TSL<TS_T>>>(output);
    target.copy_tick(source);
}

void FixedTimeSeriesListRuntimeType::copy(const TimeSeriesInputData &input, OutputState &output) const {
    if (input.valid()) {
        const auto *source_type = type_cast<TimeSeriesListType>(&input.runtime_type().type());
        if (source_type == nullptr || source_type->item_count() != item_count(output.data())) {
            throw std::invalid_argument{"time series list copy requires a matching item count"};
        }
    }

    auto source = In<TSL<TS_T>>{input};
    auto target = output_factory::make<Out<TSL<TS_T>>>(output);
    target.copy(source);
}

Node free_list_node(const free_fixed_time_series_list_storage &storage) noexcept {
    return Node::from_output(*storage.node, const_cast<free_fixed_time_series_list_storage *>(&storage));
}

bool free_list_valid(const free_fixed_time_series_list_storage &storage) noexcept {
    if (storage.node == nullptr) {
        return false;
    }

    const auto node = free_list_node(storage);
    if (node.input_count() == 0) {
        return true;
    }

    for (auto index = std::size_t{}; index < node.input_count(); ++index) {
        if (node.input_last_updated(index).valid()) {
            return true;
        }
    }
    return false;
}

FreeFixedTimeSeriesListRuntimeType::FreeFixedTimeSeriesListRuntimeType(const TimeSeriesListType &type)
    : TimeSeriesRuntimeType{type,
                            sizeof(free_fixed_time_series_list_storage),
                            alignof(free_fixed_time_series_list_storage),
                            type.item_count() > util::index<>::inline_capacity,
                            false,
                            true},
      item_runtime_type_{&resolve_time_series_runtime_type(type.item_type())} {}

bool FreeFixedTimeSeriesListRuntimeType::collection_valid(const void *value) const noexcept {
    return free_list_valid(*static_cast<const free_fixed_time_series_list_storage *>(value));
}

std::size_t FreeFixedTimeSeriesListRuntimeType::item_count(const void *) const {
    return static_cast<const TimeSeriesListType &>(type()).item_count();
}

TimeSeriesRuntimeType::ConstItem FreeFixedTimeSeriesListRuntimeType::item(const void *value, item_id_t id) const {
    if (id >= item_count(value)) {
        throw std::out_of_range{"time series list item index is out of range"};
    }

    const auto &storage = *static_cast<const free_fixed_time_series_list_storage *>(value);
    const auto node = free_list_node(storage);
    if (storage.node->parameters()[id].mode() == ParameterBindingMode::reference) {
        if (const auto *sample = node.reference_input_sample(id); sample != nullptr && !sample->empty()) {
            return sample->input();
        }
        const auto *reference_input = node.reference_input(id);
        if (reference_input->reference()) {
            const auto target = ResolvedTimeSeries::resolve(reference_input->reference());
            return {*target.runtime_type, target.last_updated, target.storage};
        }
    }

    const auto input = node.time_series_input(id);
    return {input.runtime_type(), node.input_last_updated(id), input.data()};
}

void FreeFixedTimeSeriesListRuntimeType::for_each_item_id(const void *value, void *context,
                                                         ItemIdFunction function) const {
    for (auto id = item_id_t{}; id < item_count(value); ++id) {
        function(context, id);
    }
}

bool FreeFixedTimeSeriesListRuntimeType::item_updated(const TimeSeriesState &last_updated, const void *value,
                                                      item_id_t id) const noexcept {
    const auto &storage = *static_cast<const free_fixed_time_series_list_storage *>(value);
    return last_updated.valid() && storage.change_time == last_updated.time() && storage.updated.contains(id);
}

void FreeFixedTimeSeriesListRuntimeType::for_each_updated_item(const TimeSeriesState &last_updated,
                                                               const void *value, void *context,
                                                               ItemIdFunction function) const {
    const auto &storage = *static_cast<const free_fixed_time_series_list_storage *>(value);
    if (!last_updated.valid() || storage.change_time != last_updated.time()) {
        return;
    }
    for (const auto index : storage.updated) {
        function(context, index);
    }
}

void FreeFixedTimeSeriesListRuntimeType::for_each_modified_item(
    const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time, void *context,
    ModifiedItemFunction function) const {
    const auto &storage = *static_cast<const free_fixed_time_series_list_storage *>(value);
    if (storage.node == nullptr) {
        return;
    }

    const auto node = free_list_node(storage);
    if (!last_updated.valid()) {
        for (auto index = std::size_t{}; index < node.input_count(); ++index) {
            const auto input = node.time_series_input(index);
            function(context, {static_cast<item_id_t>(index), &input.runtime_type(), &last_updated, input.data()});
        }
        return;
    }
    if (storage.change_time != evaluation_time) {
        return;
    }
    for (const auto index : storage.updated) {
        const auto item = this->item(value, static_cast<item_id_t>(index));
        function(context, {static_cast<item_id_t>(index), &item.runtime_type, &item.last_updated, item.value});
    }
}

void FreeFixedTimeSeriesListRuntimeType::construct_initial_value(void *destination) const noexcept {
    std::construct_at(static_cast<free_fixed_time_series_list_storage *>(destination));
}

void FreeFixedTimeSeriesListRuntimeType::destroy_value(void *value, allocator &memory) const noexcept {
    auto *storage = static_cast<free_fixed_time_series_list_storage *>(value);
    storage->updated.release(memory);
    std::destroy_at(storage);
}

const TimeSeriesRuntimeType &free_fixed_time_series_list_runtime_type(const TimeSeriesListType &type) {
    static auto mutex = util::static_storage<std::mutex>{};
    static auto runtime_types =
        util::static_storage<std::vector<std::unique_ptr<FreeFixedTimeSeriesListRuntimeType>>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = runtime_types.get();
    const auto existing = std::ranges::find_if(
        cache, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != cache.end()) {
        return **existing;
    }

    cache.push_back(std::make_unique<FreeFixedTimeSeriesListRuntimeType>(type));
    return *cache.back();
}

const TimeSeriesRuntimeType &
fixed_time_series_list_runtime_type(const TimeSeriesListType &type, const TimeSeriesRuntimeType &item) {
    struct Entry {
        const TimeSeriesListType *type;
        const TimeSeriesRuntimeType *item;
        std::unique_ptr<FixedTimeSeriesListRuntimeType> runtime_type;
    };

    static auto mutex = util::static_storage<std::mutex>{};
    static auto entries = util::static_storage<std::vector<Entry>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = entries.get();
    const auto found =
        std::ranges::find_if(cache, [&](const auto &entry) { return entry.type == &type && entry.item == &item; });
    if (found != cache.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<FixedTimeSeriesListRuntimeType>(type, item);
    auto &result = *runtime_type;
    cache.push_back({&type, &item, std::move(runtime_type)});
    return result;
}

} // namespace bg::details
