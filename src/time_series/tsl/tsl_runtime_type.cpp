#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_free_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

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
                            true} {}

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
    const auto publish = [&](std::size_t index, const TimeSeriesState &item_last_updated) {
        const auto input = node.time_series_input(index);
        function(context, {static_cast<item_id_t>(index), &input.runtime_type(), &item_last_updated, input.data()});
    };

    if (!last_updated.valid()) {
        for (auto index = std::size_t{}; index < node.input_count(); ++index) {
            publish(index, last_updated);
        }
        return;
    }
    if (storage.change_time != evaluation_time) {
        return;
    }
    for (const auto index : storage.updated) {
        publish(index, node.input_last_updated(index));
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
    static auto mutex = std::mutex{};
    static auto runtime_types = std::vector<std::unique_ptr<FreeFixedTimeSeriesListRuntimeType>>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find_if(
        runtime_types, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != runtime_types.end()) {
        return **existing;
    }

    runtime_types.push_back(std::make_unique<FreeFixedTimeSeriesListRuntimeType>(type));
    return *runtime_types.back();
}

const TimeSeriesRuntimeType &
fixed_time_series_list_runtime_type(const TimeSeriesListType &type, const TimeSeriesRuntimeType &item) {
    struct Entry {
        const TimeSeriesListType *type;
        const TimeSeriesRuntimeType *item;
        std::unique_ptr<FixedTimeSeriesListRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found =
        std::ranges::find_if(entries, [&](const auto &entry) { return entry.type == &type && entry.item == &item; });
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<FixedTimeSeriesListRuntimeType>(type, item);
    auto &result = *runtime_type;
    entries.push_back({&type, &item, std::move(runtime_type)});
    return result;
}

} // namespace bg::details
