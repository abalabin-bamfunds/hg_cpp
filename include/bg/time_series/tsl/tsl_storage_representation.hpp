#pragma once

#include <bg/time_series/cpp_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_input_view.hpp>
#include <bg/time_series/tsl/tsl_output_view.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/util/polymorphic_value.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

class FixedTimeSeriesListStorageRepresentation final : public TimeSeriesStorageRepresentation {
  public:
    FixedTimeSeriesListStorageRepresentation(const TimeSeriesListType &type,
                                             const TimeSeriesStorageRepresentation &item) noexcept
        : type_{&type}, item_{&item}, alignment_{std::max({alignof(time_series_list_storage), alignof(EvaluationTime),
                                                         item.value_alignment()})},
          first_item_offset_{align_up(sizeof(time_series_list_storage), alignment_)},
          value_offset_{align_up(sizeof(EvaluationTime), item.value_alignment())},
          item_stride_{align_up(value_offset_ + item.value_size(), alignment_)},
          size_{first_item_offset_ + type.item_count() * item_stride_},
          input_{util::polymorphic_value<TimeSeriesInputView>::copy(RuntimeTimeSeriesListInputView{*this})},
          output_{util::polymorphic_value<TimeSeriesOutputView>::copy(RuntimeTimeSeriesListOutputView{*this})} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return size_; }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignment_; }
    [[nodiscard]] bool uses_variable_memory() const noexcept override {
        return type_->item_count() > util::index<>::inline_capacity || item_->uses_variable_memory();
    }
    [[nodiscard]] bool requires_finalization() const noexcept override { return item_->requires_finalization(); }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override {
        return item_->supports_byte_copy_initialization();
    }
    [[nodiscard]] const TimeSeriesStorageRepresentation *contained_item_representation() const noexcept override {
        return item_;
    }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override { return input_.get(); }
    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override {
        return &output_.get();
    }

    [[nodiscard]] FixedItem fixed_item(std::size_t index) const override {
        if (index >= type_->item_count()) {
            throw std::out_of_range{"time series list item index is out of range"};
        }

        const auto item = first_item_offset_ + index * item_stride_;
        return {*item_, item, item + value_offset_};
    }

    [[nodiscard]] bool mark_item_modified(void *value, std::size_t item, EvaluationTime evaluation_time,
                                          EvaluationTime, std::pmr::memory_resource &memory) const noexcept override {
        return static_cast<time_series_list_storage *>(value)->mark(item, evaluation_time, memory);
    }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        const auto *bytes = static_cast<const std::byte *>(value);
        for (auto index = std::size_t{}; index < type_->item_count(); ++index) {
            if (*reinterpret_cast<const EvaluationTime *>(bytes + fixed_item(index).last_updated_offset) > MIN_DT) {
                return true;
            }
        }
        return false;
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto *bytes = static_cast<const std::byte *>(value);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto field = fixed_item(index);
            function(context, {index, item_, &item_last_updated, bytes + field.value_offset});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < type_->item_count(); ++index) {
                publish(index, MIN_DT);
            }
            return;
        }

        const auto &storage = *static_cast<const time_series_list_storage *>(value);
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.updated) {
            const auto field = fixed_item(index);
            publish(index, *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset));
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_list_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        for (auto index = std::size_t{}; index < type_->item_count(); ++index) {
            const auto field = fixed_item(index);
            std::construct_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset), MIN_DT);
            item_->construct_initial_value(bytes + field.value_offset);
        }
    }

    void construct_runtime_value(void *destination, std::pmr::memory_resource &memory) const override {
        auto *storage = std::construct_at(static_cast<time_series_list_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        auto constructed = std::size_t{};
        auto rollback = util::scope_exit{[&]() noexcept {
            while (constructed != 0) {
                const auto field = fixed_item(--constructed);
                item_->destroy_value(bytes + field.value_offset, memory);
                std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset));
            }
            std::destroy_at(storage);
        }};

        for (; constructed < type_->item_count(); ++constructed) {
            const auto field = fixed_item(constructed);
            std::construct_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset), MIN_DT);
            item_->construct_runtime_value(bytes + field.value_offset, memory);
        }
        rollback.release();
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *bytes = static_cast<std::byte *>(value);
        for (auto index = type_->item_count(); index != 0; --index) {
            const auto field = fixed_item(index - 1);
            item_->destroy_value(bytes + field.value_offset, memory);
            std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset));
        }
        auto *storage = static_cast<time_series_list_storage *>(value);
        storage->updated.release(memory);
        std::destroy_at(storage);
    }

  private:
    [[nodiscard]] static std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept {
        const auto remainder = offset % alignment;
        return remainder == 0 ? offset : offset + alignment - remainder;
    }

    const TimeSeriesListType *type_;
    const TimeSeriesStorageRepresentation *item_;
    std::size_t alignment_;
    std::size_t first_item_offset_;
    std::size_t value_offset_;
    std::size_t item_stride_;
    std::size_t size_;
    util::polymorphic_value<TimeSeriesInputView> input_;
    util::polymorphic_value<TimeSeriesOutputView> output_;
};

[[nodiscard]] inline const TimeSeriesStorageRepresentation &
fixed_time_series_list_storage_representation(const TimeSeriesListType &type,
                                              const TimeSeriesStorageRepresentation &item) {
    struct Entry {
        const TimeSeriesListType *type;
        const TimeSeriesStorageRepresentation *item;
        std::unique_ptr<FixedTimeSeriesListStorageRepresentation> representation;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.type == &type && entry.item == &item;
    });
    if (found != entries.end()) {
        return *found->representation;
    }

    auto representation = std::make_unique<FixedTimeSeriesListStorageRepresentation>(type, item);
    auto &result = *representation;
    entries.push_back({&type, &item, std::move(representation)});
    return result;
}

template <typename Item, std::size_t Size, auto... Properties>
struct CppTimeSeriesStorageRepresentation<TSL<Item, Size, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        const auto &list = static_cast<const TimeSeriesListType &>(type);
        return fixed_time_series_list_storage_representation(
            list, CppTimeSeriesStorageRepresentation<Item>::get(list.item_type()));
    }
};

} // namespace bg::details
