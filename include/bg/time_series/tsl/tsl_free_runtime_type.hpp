#pragma once

#include <bg/graph/node.hpp>
#include <bg/time_series/tsl/tsl_free_storage.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {

template <std::size_t Size>
[[nodiscard]] Node free_list_node(const free_fixed_time_series_list_storage<Size> &storage) noexcept {
    return Node::from_output(*storage.node, const_cast<free_fixed_time_series_list_storage<Size> *>(&storage));
}

template <std::size_t Size>
[[nodiscard]] bool free_list_valid(const free_fixed_time_series_list_storage<Size> &storage) noexcept {
    if (storage.node == nullptr) {
        return false;
    }

    const auto node = free_list_node(storage);
    for (auto index = std::size_t{}; index < node.input_count(); ++index) {
        if (node.input_last_updated(index) > MIN_DT) {
            return true;
        }
    }
    return false;
}

template <std::size_t Size> class FreeFixedTimeSeriesListRuntimeType final : public TimeSeriesRuntimeType {
    using Storage = free_fixed_time_series_list_storage<Size>;

  public:
    explicit FreeFixedTimeSeriesListRuntimeType(const TimeSeriesListType &type)
        : TimeSeriesRuntimeType{type, sizeof(Storage), alignof(Storage), false, false, true} {}

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        const auto &storage = *static_cast<const Storage *>(value);
        return free_list_valid(storage);
    }

    [[nodiscard]] std::size_t item_count(const void *) const override { return Size; }

    [[nodiscard]] ConstItem item(const void *value, TimeSeriesItemId id) const override {
        if (id >= Size) {
            throw std::out_of_range{"time series list item index is out of range"};
        }

        const auto &storage = *static_cast<const Storage *>(value);
        const auto input = free_list_node(storage).time_series_input(id, id);
        return {input.runtime_type(), input.last_updated(), input.data()};
    }

    void for_each_item_id(const void *, void *context, ItemIdFunction function) const override {
        for (auto id = TimeSeriesItemId{}; id < Size; ++id) {
            function(context, id);
        }
    }

    [[nodiscard]] bool item_updated(const EvaluationTime &last_updated, const void *value,
                                    TimeSeriesItemId id) const noexcept override {
        return last_updated > MIN_DT && static_cast<const Storage *>(value)->changes.contains(id, last_updated);
    }

    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &changes = static_cast<const Storage *>(value)->changes;
        if (last_updated <= MIN_DT || changes.time() != last_updated) {
            return;
        }
        for (const auto index : changes) {
            function(context, index);
        }
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        if (storage.node == nullptr) {
            return;
        }

        const auto node = free_list_node(storage);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto input = node.time_series_input(index, index);
            function(context, {static_cast<TimeSeriesItemId>(index), &input.runtime_type(), &item_last_updated,
                               input.data()});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < Size; ++index) {
                publish(index, MIN_DT);
            }
            return;
        }
        if (storage.changes.time() != evaluation_time) {
            return;
        }

        for (const auto index : storage.changes) {
            publish(index, node.input_last_updated(index));
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &) const noexcept override {
        std::destroy_at(static_cast<Storage *>(value));
    }
};

template <std::size_t Size>
[[nodiscard]] const TimeSeriesRuntimeType &free_fixed_time_series_list_runtime_type(const TimeSeriesListType &type) {
    using RuntimeType = FreeFixedTimeSeriesListRuntimeType<Size>;
    static auto mutex = std::mutex{};
    static auto runtime_types = std::vector<std::unique_ptr<RuntimeType>>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing =
        std::ranges::find_if(runtime_types, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != runtime_types.end()) {
        return **existing;
    }

    runtime_types.push_back(std::make_unique<RuntimeType>(type));
    return *runtime_types.back();
}

} // namespace bg::details
