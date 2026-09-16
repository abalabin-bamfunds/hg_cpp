#pragma once

#include <bg/graph/node.hpp>
#include <bg/time_series/tsb/tsb_free_storage.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <memory>
#include <stdexcept>

namespace bg::details {

Node free_bundle_node(const free_time_series_bundle_storage &storage) noexcept;
bool free_bundle_valid(const free_time_series_bundle_storage &storage) noexcept;

class FreeTimeSeriesBundleRuntimeType final : public TimeSeriesRuntimeType {
  public:
    explicit FreeTimeSeriesBundleRuntimeType(const TimeSeriesBundleType &type)
        : TimeSeriesRuntimeType{type,
                                sizeof(free_time_series_bundle_storage),
                                alignof(free_time_series_bundle_storage),
                                type.item_count() > util::index<>::inline_capacity,
                                false,
                                true} {}

    bool collection_valid(const void *value) const noexcept override {
        return free_bundle_valid(*static_cast<const free_time_series_bundle_storage *>(value));
    }

    std::size_t item_count(const void *value) const override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        return storage.node == nullptr ? 0 : free_bundle_node(storage).input_count();
    }

    ConstItem item(const void *value, item_id_t id) const override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        if (storage.node == nullptr || id >= free_bundle_node(storage).input_count()) {
            throw std::out_of_range{"time series bundle item index is out of range"};
        }
        const auto node = free_bundle_node(storage);
        const auto input = node.time_series_input(id, id);
        return {input.runtime_type(), node.input_last_updated(id), input.data()};
    }

    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override {
        const auto count = item_count(value);
        for (auto id = item_id_t{}; id < count; ++id) {
            function(context, id);
        }
    }

    bool item_updated(const EvaluationTime &last_updated, const void *value,
                                    item_id_t id) const noexcept override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        return last_updated > MIN_DT && storage.change_time == last_updated && storage.modified.contains(id);
    }

    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        if (last_updated <= MIN_DT || storage.change_time != last_updated) {
            return;
        }
        for (const auto index : storage.modified) {
            function(context, index);
        }
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        if (storage.node == nullptr) {
            return;
        }
        const auto node = free_bundle_node(storage);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto input = node.time_series_input(index, index);
            function(context, {static_cast<item_id_t>(index), &input.runtime_type(), &item_last_updated,
                               input.data()});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < node.input_count(); ++index) {
                publish(index, MIN_DT);
            }
            return;
        }
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.modified) {
            publish(index, node.input_last_updated(index));
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<free_time_series_bundle_storage *>(destination));
    }

    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto *storage = static_cast<free_time_series_bundle_storage *>(value);
        storage->modified.release(memory);
        std::destroy_at(storage);
    }
};

const TimeSeriesRuntimeType &free_time_series_bundle_runtime_type(const TimeSeriesBundleType &type);

} // namespace bg::details
