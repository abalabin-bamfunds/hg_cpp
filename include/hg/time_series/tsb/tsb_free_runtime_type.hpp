#pragma once

#include <bg/graph/node.hpp>
#include <bg/time_series/tsb/tsb_free_storage.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <memory>
#include <stdexcept>
#include <vector>

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
                                true} {
        item_runtime_types_.reserve(type.item_count());
        for (auto index = std::size_t{}; index < type.item_count(); ++index) {
            item_runtime_types_.push_back(&resolve_time_series_runtime_type(type.item_type(index)));
        }
    }

    bool collection_valid(const void *value) const noexcept override {
        return free_bundle_valid(*static_cast<const free_time_series_bundle_storage *>(value));
    }

    const ReferenceRepresentationFactory *reference_representation_factory() const noexcept override;

    const TimeSeriesRuntimeType &item_runtime_type(item_id_t id) const override {
        const auto &bundle = static_cast<const TimeSeriesBundleType &>(type());
        if (id >= bundle.item_count()) {
            throw std::out_of_range{"time series bundle item index is out of range"};
        }
        return *item_runtime_types_[id];
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

    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override {
        const auto count = item_count(value);
        for (auto id = item_id_t{}; id < count; ++id) {
            function(context, id);
        }
    }

    bool item_updated(const TimeSeriesState &last_updated, const void *value,
                      item_id_t id) const noexcept override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        return last_updated.valid() && storage.change_time == last_updated.time() && storage.modified.contains(id);
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        if (!last_updated.valid() || storage.change_time != last_updated.time()) {
            return;
        }
        for (const auto index : storage.modified) {
            function(context, index);
        }
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const free_time_series_bundle_storage *>(value);
        if (storage.node == nullptr) {
            return;
        }
        const auto node = free_bundle_node(storage);
        if (!last_updated.valid()) {
            for (auto index = std::size_t{}; index < node.input_count(); ++index) {
                const auto input = node.time_series_input(index);
                function(context, {static_cast<item_id_t>(index), &input.runtime_type(), &last_updated,
                                   input.data()});
            }
            return;
        }
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.modified) {
            const auto item = this->item(value, static_cast<item_id_t>(index));
            function(context, {static_cast<item_id_t>(index), &item.runtime_type, &item.last_updated, item.value});
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

  private:
    std::vector<const TimeSeriesRuntimeType *> item_runtime_types_;
};

const TimeSeriesRuntimeType &free_time_series_bundle_runtime_type(const TimeSeriesBundleType &type);

} // namespace bg::details
