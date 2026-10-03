#pragma once

#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_storage.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

namespace bg::details {

class TimeSeriesBundleRuntimeTypeBase : public TimeSeriesRuntimeType {
  public:
    TimeSeriesBundleRuntimeTypeBase(const TimeSeriesBundleType &type,
                                    std::span<const TimeSeriesRuntimeType *const> items)
        : TimeSeriesRuntimeType{type} {
        auto offset = sizeof(time_series_bundle_storage);
        auto alignment = alignof(time_series_bundle_storage);
        fields_.reserve(items.size());

        for (const auto *item : items) {
            alignment = std::max({alignment, alignof(EvaluationTime), item->value_alignment()});
            offset = align_up(offset, alignof(EvaluationTime));
            const auto last_updated = offset;
            offset += sizeof(EvaluationTime);
            offset = align_up(offset, item->value_alignment());
            fields_.push_back({*item, last_updated, offset});
            offset += item->value_size();
        }

        set_storage_layout(align_up(offset, alignment), alignment,
                           items.size() > util::index<>::inline_capacity ||
                               std::ranges::any_of(items, &TimeSeriesRuntimeType::uses_variable_memory),
                           std::ranges::any_of(items, &TimeSeriesRuntimeType::requires_finalization),
                           std::ranges::all_of(items, &TimeSeriesRuntimeType::supports_byte_copy_initialization));
    }

    ItemLayout item_layout(item_id_t id) const final { return fields_.at(id); }

    std::size_t item_count(const void *) const final { return fields_.size(); }

    ConstItem item(const void *value, item_id_t id) const final {
        const auto &field = fields_.at(id);
        const auto *bytes = static_cast<const std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }

    MutableItem item(void *value, item_id_t id) const final {
        const auto &field = fields_.at(id);
        auto *bytes = static_cast<std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }

    void for_each_item_id(const void *, void *context, ItemIdFunction function) const final {
        for (auto id = item_id_t{}; id < fields_.size(); ++id) {
            function(context, id);
        }
    }

    bool item_updated(const TimeSeriesState &last_updated, const void *value,
                                    item_id_t id) const noexcept final {
        const auto &storage = *static_cast<const time_series_bundle_storage *>(value);
        return last_updated.valid() && storage.change_time == last_updated.time() && storage.modified.contains(id);
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const final {
        const auto &storage = *static_cast<const time_series_bundle_storage *>(value);
        if (!last_updated.valid() || storage.change_time != last_updated.time()) {
            return;
        }
        for (const auto index : storage.modified) {
            function(context, index);
        }
    }

    void invalidate_output(const OutputState &output) const final { invalidate_output_items(output); }

    [[nodiscard]] bool mark_item_modified(void *value, item_id_t id, EvaluationTime evaluation_time,
                                          const TimeSeriesState &, allocator &memory) const noexcept final {
        auto &storage = *static_cast<time_series_bundle_storage *>(value);
        if (storage.change_time != evaluation_time) {
            storage.modified.clear();
            storage.change_time = evaluation_time;
        }
        return storage.modified.set(id, memory);
    }

    bool collection_valid(const void *value) const noexcept final {
        const auto *bytes = static_cast<const std::byte *>(value);
        return std::ranges::any_of(fields_, [&](const auto &field) {
            return reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset)->valid();
        });
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const final {
        const auto *bytes = static_cast<const std::byte *>(value);
        const auto &storage = *static_cast<const time_series_bundle_storage *>(value);
        const auto publish = [&](std::size_t index, const TimeSeriesState &item_last_updated) {
            const auto &field = fields_[index];
            function(context, {static_cast<item_id_t>(index), &field.runtime_type, &item_last_updated,
                               bytes + field.value_offset});
        };

        if (!last_updated.valid()) {
            for (auto index = std::size_t{}; index < fields_.size(); ++index) {
                publish(index, last_updated);
            }
            return;
        }
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.modified) {
            const auto &field = fields_[index];
            publish(index, *reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset));
        }
    }

    void construct_initial_value(void *destination) const noexcept final {
        std::construct_at(static_cast<time_series_bundle_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        for (const auto &field : fields_) {
            std::construct_at(reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset));
            field.runtime_type.construct_initial_value(bytes + field.value_offset);
        }
    }

    void construct_runtime_value(void *destination, allocator &memory) const final {
        auto *storage = std::construct_at(static_cast<time_series_bundle_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        auto constructed = std::size_t{};
        auto rollback = util::scope_exit{[&]() noexcept {
            while (constructed != 0) {
                --constructed;
                const auto &field = fields_[constructed];
                field.runtime_type.destroy_value(bytes + field.value_offset, memory);
                std::destroy_at(reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset));
            }
            std::destroy_at(storage);
        }};
        for (const auto &field : fields_) {
            std::construct_at(reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset));
            field.runtime_type.construct_runtime_value(bytes + field.value_offset, memory);
            ++constructed;
        }
        rollback.release();
    }

    void destroy_value(void *value, allocator &memory) const noexcept final {
        auto *bytes = static_cast<std::byte *>(value);
        for (auto field = fields_.rbegin(); field != fields_.rend(); ++field) {
            field->runtime_type.destroy_value(bytes + field->value_offset, memory);
            std::destroy_at(reinterpret_cast<TimeSeriesState *>(bytes + field->last_updated_offset));
        }
        auto *storage = static_cast<time_series_bundle_storage *>(value);
        storage->modified.release(memory);
        std::destroy_at(storage);
    }

  protected:
    [[nodiscard]] static std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept {
        const auto remainder = offset % alignment;
        return remainder == 0 ? offset : offset + alignment - remainder;
    }

  private:
    std::vector<ItemLayout> fields_;
};

class RuntimeTimeSeriesBundleRuntimeType final : public TimeSeriesBundleRuntimeTypeBase {
  public:
    RuntimeTimeSeriesBundleRuntimeType(const TimeSeriesBundleType &type,
                                       std::span<const TimeSeriesRuntimeType *const> items)
        : TimeSeriesBundleRuntimeTypeBase{type, items} {}
};

const TimeSeriesRuntimeType &
time_series_bundle_runtime_type(const TimeSeriesBundleType &type,
                                std::span<const TimeSeriesRuntimeType *const> items);

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
void append_bundle_item_runtime_types(const TimeSeriesBundleType &type,
                                      std::vector<const TimeSeriesRuntimeType *> &items,
                                      std::index_sequence<Index...>) {
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        append_bundle_item_runtime_types<Base::template apply>(
            type, items, std::make_index_sequence<direct_bundle_field_count<Base::template apply>>{});
    }
    const auto append = [&]<std::size_t Field>() {
        constexpr auto Item = direct_bundle_field_logical_index<Field, Schema>;
        const auto *runtime_type =
            &::bg::cpp_time_series_runtime_type<direct_bundle_field_time_series<Field, Schema>>(type.item_type(Item));
        if (Item < items.size()) {
            items[Item] = runtime_type;
        } else {
            items.push_back(runtime_type);
        }
    };
    (append.template operator()<Index>(), ...);
}

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct CppTimeSeriesRuntimeType<TSB<Schema, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        const auto &bundle = static_cast<const TimeSeriesBundleType &>(type);
        auto items = std::vector<const TimeSeriesRuntimeType *>{};
        items.reserve(bundle.item_count());
        append_bundle_item_runtime_types<Schema>(bundle, items,
                                                 std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        return time_series_bundle_runtime_type(bundle, items);
    }
};

} // namespace bg::details
