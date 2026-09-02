#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/time_series/tsb/tsb_alias_storage.hpp>
#include <bg/time_series/tsb/tsb_input_view.hpp>

#include <ranges>
#include <stdexcept>

namespace bg::details {

[[nodiscard]] inline const std::byte *
aliased_bundle_runtime_storage(const aliased_time_series_bundle_storage &storage) noexcept {
    return reinterpret_cast<const std::byte *>(&storage) - storage.node->output_value_offset();
}

[[nodiscard]] inline const TimeSeriesStorageRepresentation &
aliased_bundle_item_representation(const aliased_time_series_bundle_storage &storage, const ParameterBinding &binding,
                                   std::size_t index) noexcept {
    const auto *runtime_storage = aliased_bundle_runtime_storage(storage);
    if (binding.mode() == ParameterBindingMode::reference) {
        const auto *target = binding.reference_input(runtime_storage).target;
        if (target != nullptr) {
            return *target->target_representation;
        }
    }
    return storage.node->time_series_input_storage_representation(index);
}

[[nodiscard]] inline bool aliased_bundle_valid(const aliased_time_series_bundle_storage &storage) noexcept {
    if (storage.node == nullptr) {
        return false;
    }
    const auto *runtime_storage = aliased_bundle_runtime_storage(storage);
    return std::ranges::any_of(storage.node->parameters(),
                               [&](const auto &binding) { return binding.last_updated(runtime_storage) > MIN_DT; });
}

class AliasedTimeSeriesBundleInputView final : public TimeSeriesBundleInputView {
  public:
    [[nodiscard]] std::size_t size() const noexcept override {
        const auto &storage = *static_cast<const aliased_time_series_bundle_storage *>(value_);
        return storage.node == nullptr ? 0 : storage.node->parameters().size();
    }

    [[nodiscard]] Item item(std::size_t index) const override {
        const auto &storage = *static_cast<const aliased_time_series_bundle_storage *>(value_);
        if (storage.node == nullptr || index >= storage.node->parameters().size()) {
            throw std::out_of_range{"time series bundle item index is out of range"};
        }
        const auto *runtime_storage = aliased_bundle_runtime_storage(storage);
        const auto &binding = storage.node->parameters()[index];
        const auto &representation = aliased_bundle_item_representation(storage, binding, index);
        return {representation.input_view_prototype(), binding.last_updated(runtime_storage),
                binding.value(runtime_storage)};
    }

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept override {
        const auto &storage = *static_cast<const aliased_time_series_bundle_storage *>(value_);
        return valid() && storage.change_time == last_updated() && storage.modified.contains(index);
    }

    void for_each_updated(void *context, void (*function)(void *, std::size_t)) const override {
        const auto &storage = *static_cast<const aliased_time_series_bundle_storage *>(value_);
        if (!valid() || storage.change_time != last_updated()) {
            return;
        }
        for (const auto index : storage.modified) {
            function(context, index);
        }
    }
};

static_assert(sizeof(AliasedTimeSeriesBundleInputView) == sizeof(TimeSeriesBundleInputView));

} // namespace bg::details
