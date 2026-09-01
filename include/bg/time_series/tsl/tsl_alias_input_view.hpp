#pragma once

#include <bg/graph/compiled_node.hpp>
#include <bg/time_series/tsl/tsl_alias_storage.hpp>
#include <bg/time_series/tsl/tsl_input_view.hpp>

#include <cstddef>
#include <ranges>
#include <stdexcept>

namespace bg {

namespace details {

template <std::size_t Size>
[[nodiscard]] const std::byte *
aliased_list_runtime_storage(const aliased_fixed_time_series_list_storage<Size> &storage) noexcept {
    return reinterpret_cast<const std::byte *>(&storage) - storage.node->output_value_offset();
}

template <std::size_t Size>
[[nodiscard]] const TimeSeriesStorageRepresentation &
aliased_list_item_representation(const aliased_fixed_time_series_list_storage<Size> &storage,
                                 const ParameterBinding &binding, std::size_t index) noexcept {
    const auto *runtime_storage = aliased_list_runtime_storage(storage);
    if (binding.mode() == ParameterBindingMode::reference) {
        const auto *target = binding.reference_input(runtime_storage).target;
        if (target != nullptr) {
            return *target->target_representation;
        }
    }

    return storage.node->time_series_input_storage_representation(index);
}

template <std::size_t Size>
[[nodiscard]] bool aliased_list_valid(const aliased_fixed_time_series_list_storage<Size> &storage) noexcept {
    if (storage.node == nullptr) {
        return false;
    }

    const auto *runtime_storage = aliased_list_runtime_storage(storage);
    return std::ranges::any_of(storage.node->parameters(),
                               [&](const auto &binding) { return binding.last_updated(runtime_storage) > MIN_DT; });
}

} // namespace details

template <std::size_t Size> class AliasedFixedTimeSeriesListInputView final : public TimeSeriesListInputView {
    using Storage = details::aliased_fixed_time_series_list_storage<Size>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return Size; }

    [[nodiscard]] TimeSeriesListInputView::Item item(std::size_t index) const override {
        if (index >= Size) {
            throw std::out_of_range{"time series list item index is out of range"};
        }

        const auto &storage = *static_cast<const Storage *>(value_);
        const auto *runtime_storage = details::aliased_list_runtime_storage(storage);
        const auto &binding = storage.node->parameters()[index];
        const auto &representation = details::aliased_list_item_representation(storage, binding, index);

        return {representation.input_view_prototype(), binding.last_updated(runtime_storage),
                binding.value(runtime_storage)};
    }

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept override {
        return valid() && static_cast<const Storage *>(value_)->changes.contains(index, last_updated());
    }

    void for_each_updated(void *context, void (*function)(void *, std::size_t)) const override {
        const auto &changes = static_cast<const Storage *>(value_)->changes;
        if (!valid() || changes.time() != last_updated()) {
            return;
        }

        for (const auto index : changes) {
            function(context, index);
        }
    }
};

static_assert(sizeof(AliasedFixedTimeSeriesListInputView<1>) == sizeof(TimeSeriesListInputView));

} // namespace bg
