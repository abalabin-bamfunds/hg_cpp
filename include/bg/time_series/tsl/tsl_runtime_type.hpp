#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace bg::details {

class FixedTimeSeriesListRuntimeType final : public TimeSeriesRuntimeType {
    struct Layout {
        std::size_t size;
        std::size_t alignment;
        std::size_t first_item_offset;
        std::size_t value_offset;
        std::size_t item_stride;
    };

  public:
    FixedTimeSeriesListRuntimeType(const TimeSeriesListType &type, const TimeSeriesRuntimeType &item) noexcept
        : FixedTimeSeriesListRuntimeType{type, item, make_layout(type, item)} {}

    ItemLayout item_layout(item_id_t id) const override {
        if (id >= list_type().item_count()) {
            throw std::out_of_range{"time series list item index is out of range"};
        }

        const auto item = first_item_offset_ + id * item_stride_;
        return {*item_, item, item + value_offset_};
    }

    std::size_t item_count(const void *) const override { return list_type().item_count(); }

    ConstItem item(const void *value, item_id_t id) const override {
        const auto field = item_layout(id);
        const auto *bytes = static_cast<const std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }

    MutableItem item(void *value, item_id_t id) const override {
        const auto field = item_layout(id);
        auto *bytes = static_cast<std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }

    void for_each_item_id(const void *, void *context, ItemIdFunction function) const override {
        for (auto id = item_id_t{}; id < list_type().item_count(); ++id) {
            function(context, id);
        }
    }

    bool item_updated(const EvaluationTime &last_updated, const void *value,
                                    item_id_t id) const noexcept override {
        const auto &storage = *static_cast<const time_series_list_storage *>(value);
        return last_updated > MIN_DT && storage.change_time == last_updated && storage.updated.contains(id);
    }

    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const time_series_list_storage *>(value);
        if (last_updated <= MIN_DT || storage.change_time != last_updated) {
            return;
        }
        for (const auto index : storage.updated) {
            function(context, index);
        }
    }

    void invalidate_output(const details::OutputState &output) const override { invalidate_output_items(output); }

    [[nodiscard]] bool mark_item_modified(void *value, item_id_t id, EvaluationTime evaluation_time,
                                          EvaluationTime, allocator &memory) const noexcept override {
        return static_cast<time_series_list_storage *>(value)->mark(id, evaluation_time, memory);
    }

    bool collection_valid(const void *value) const noexcept override {
        const auto *bytes = static_cast<const std::byte *>(value);
        for (auto index = std::size_t{}; index < list_type().item_count(); ++index) {
            if (*reinterpret_cast<const EvaluationTime *>(bytes + item_layout(index).last_updated_offset) > MIN_DT) {
                return true;
            }
        }
        return false;
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto *bytes = static_cast<const std::byte *>(value);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto field = item_layout(index);
            function(context, {static_cast<item_id_t>(index), item_, &item_last_updated,
                               bytes + field.value_offset});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < list_type().item_count(); ++index) {
                publish(index, MIN_DT);
            }
            return;
        }

        const auto &storage = *static_cast<const time_series_list_storage *>(value);
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.updated) {
            const auto field = item_layout(index);
            publish(index, *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset));
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_list_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        for (auto index = std::size_t{}; index < list_type().item_count(); ++index) {
            const auto field = item_layout(index);
            std::construct_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset), MIN_DT);
            item_->construct_initial_value(bytes + field.value_offset);
        }
    }

    void construct_runtime_value(void *destination, allocator &memory) const override {
        auto *storage = std::construct_at(static_cast<time_series_list_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        auto constructed = std::size_t{};
        auto rollback = util::scope_exit{[&]() noexcept {
            while (constructed != 0) {
                const auto field = item_layout(--constructed);
                item_->destroy_value(bytes + field.value_offset, memory);
                std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset));
            }
            std::destroy_at(storage);
        }};

        for (; constructed < list_type().item_count(); ++constructed) {
            const auto field = item_layout(constructed);
            std::construct_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset), MIN_DT);
            item_->construct_runtime_value(bytes + field.value_offset, memory);
        }
        rollback.release();
    }

    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto *bytes = static_cast<std::byte *>(value);
        for (auto index = list_type().item_count(); index != 0; --index) {
            const auto field = item_layout(index - 1);
            item_->destroy_value(bytes + field.value_offset, memory);
            std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset));
        }
        auto *storage = static_cast<time_series_list_storage *>(value);
        storage->updated.release(memory);
        std::destroy_at(storage);
    }

  private:
    FixedTimeSeriesListRuntimeType(const TimeSeriesListType &type, const TimeSeriesRuntimeType &item,
                                   const Layout &layout) noexcept
        : TimeSeriesRuntimeType{type,
                                layout.size,
                                layout.alignment,
                                type.item_count() > util::index<>::inline_capacity || item.uses_variable_memory(),
                                item.requires_finalization(),
                                item.supports_byte_copy_initialization()},
          item_{&item}, first_item_offset_{static_cast<std::uint32_t>(layout.first_item_offset)},
          value_offset_{static_cast<std::uint32_t>(layout.value_offset)},
          item_stride_{static_cast<std::uint32_t>(layout.item_stride)} {}

    const TimeSeriesListType &list_type() const noexcept {
        return static_cast<const TimeSeriesListType &>(type());
    }

    [[nodiscard]] static std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept {
        const auto remainder = offset % alignment;
        return remainder == 0 ? offset : offset + alignment - remainder;
    }

    [[nodiscard]] static Layout make_layout(const TimeSeriesListType &type,
                                            const TimeSeriesRuntimeType &item) noexcept {
        const auto alignment =
            std::max({alignof(time_series_list_storage), alignof(EvaluationTime), item.value_alignment()});
        const auto first_item_offset = align_up(sizeof(time_series_list_storage), alignment);
        const auto value_offset = align_up(sizeof(EvaluationTime), item.value_alignment());
        const auto item_stride = align_up(value_offset + item.value_size(), alignment);

        return {first_item_offset + type.item_count() * item_stride, alignment, first_item_offset, value_offset,
                item_stride};
    }

    const TimeSeriesRuntimeType *item_;
    std::uint32_t first_item_offset_;
    std::uint32_t value_offset_;
    std::uint32_t item_stride_;
};

const TimeSeriesRuntimeType &
fixed_time_series_list_runtime_type(const TimeSeriesListType &type, const TimeSeriesRuntimeType &item);

template <typename Item, auto Size, auto... Properties>
struct CppTimeSeriesRuntimeType<TSL<Item, Size, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        const auto &list = static_cast<const TimeSeriesListType &>(type);
        return fixed_time_series_list_runtime_type(list, CppTimeSeriesRuntimeType<Item>::get(list.item_type()));
    }
};

} // namespace bg::details
