#pragma once

#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_storage_representation.hpp>
#include <bg/time_series/tsb/tsb_input_view.hpp>
#include <bg/time_series/tsb/tsb_output_view.hpp>
#include <bg/time_series/tsb/tsb_storage.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/util/polymorphic_value.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

namespace bg::details {

class TimeSeriesBundleStorageRepresentationBase : public TimeSeriesStorageRepresentation {
  public:
    TimeSeriesBundleStorageRepresentationBase(const TimeSeriesBundleType &type,
                                              std::span<const TimeSeriesStorageRepresentation *const> items)
        : type_{&type}, items_{items.begin(), items.end()} {
        auto offset = sizeof(time_series_bundle_storage);
        alignment_ = alignof(time_series_bundle_storage);
        fields_.reserve(items_.size());

        for (const auto *item : items_) {
            alignment_ = std::max({alignment_, alignof(EvaluationTime), item->value_alignment()});
            offset = align_up(offset, alignof(EvaluationTime));
            const auto last_updated = offset;
            offset += sizeof(EvaluationTime);
            offset = align_up(offset, item->value_alignment());
            fields_.push_back({*item, last_updated, offset});
            offset += item->value_size();
        }
        size_ = align_up(offset, alignment_);
    }

    [[nodiscard]] const TimeSeriesType &type() const noexcept final { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept final { return size_; }
    [[nodiscard]] std::size_t value_alignment() const noexcept final { return alignment_; }
    [[nodiscard]] bool uses_variable_memory() const noexcept final {
        return items_.size() > util::index<>::inline_capacity ||
               std::ranges::any_of(items_, &TimeSeriesStorageRepresentation::uses_variable_memory);
    }
    [[nodiscard]] bool requires_finalization() const noexcept final {
        return std::ranges::any_of(items_, &TimeSeriesStorageRepresentation::requires_finalization);
    }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept final {
        return std::ranges::all_of(items_, &TimeSeriesStorageRepresentation::supports_byte_copy_initialization);
    }

    [[nodiscard]] FixedItem fixed_item(std::size_t index) const final { return fields_.at(index); }

    [[nodiscard]] bool mark_item_modified(void *value, std::size_t item, EvaluationTime evaluation_time, EvaluationTime,
                                          std::pmr::memory_resource &memory) const noexcept final {
        auto &storage = *static_cast<time_series_bundle_storage *>(value);
        if (storage.change_time != evaluation_time) {
            storage.modified.clear();
            storage.change_time = evaluation_time;
        }
        return storage.modified.set(static_cast<std::uint32_t>(item), memory);
    }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept final {
        const auto *bytes = static_cast<const std::byte *>(value);
        return std::ranges::any_of(fields_, [&](const auto &field) {
            return *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset) > MIN_DT;
        });
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const final {
        const auto *bytes = static_cast<const std::byte *>(value);
        const auto &storage = *static_cast<const time_series_bundle_storage *>(value);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto &field = fields_[index];
            function(context, {index, &field.representation, &item_last_updated, bytes + field.value_offset});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < fields_.size(); ++index) {
                publish(index, MIN_DT);
            }
            return;
        }
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.modified) {
            const auto &field = fields_[index];
            publish(index, *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset));
        }
    }

    void construct_initial_value(void *destination) const noexcept final {
        std::construct_at(static_cast<time_series_bundle_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        for (const auto &field : fields_) {
            std::construct_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset), MIN_DT);
            field.representation.construct_initial_value(bytes + field.value_offset);
        }
    }

    void construct_runtime_value(void *destination, std::pmr::memory_resource &memory) const final {
        auto *storage = std::construct_at(static_cast<time_series_bundle_storage *>(destination));
        auto *bytes = static_cast<std::byte *>(destination);
        auto constructed = std::size_t{};
        auto rollback = util::scope_exit{[&]() noexcept {
            while (constructed != 0) {
                --constructed;
                const auto &field = fields_[constructed];
                field.representation.destroy_value(bytes + field.value_offset, memory);
                std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset));
            }
            std::destroy_at(storage);
        }};
        for (const auto &field : fields_) {
            std::construct_at(reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset), MIN_DT);
            field.representation.construct_runtime_value(bytes + field.value_offset, memory);
            ++constructed;
        }
        rollback.release();
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept final {
        auto *bytes = static_cast<std::byte *>(value);
        for (auto field = fields_.rbegin(); field != fields_.rend(); ++field) {
            field->representation.destroy_value(bytes + field->value_offset, memory);
            std::destroy_at(reinterpret_cast<EvaluationTime *>(bytes + field->last_updated_offset));
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
    const TimeSeriesBundleType *type_;
    std::vector<const TimeSeriesStorageRepresentation *> items_;
    std::vector<FixedItem> fields_;
    std::size_t size_{};
    std::size_t alignment_{};
};

class RuntimeTimeSeriesBundleInputView final : public TimeSeriesBundleInputView {
  public:
    explicit RuntimeTimeSeriesBundleInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesBundleInputView{representation} {}

    [[nodiscard]] std::size_t size() const noexcept override {
        return static_cast<const TimeSeriesBundleType &>(storage_representation().type()).item_count();
    }

    [[nodiscard]] Item item(std::size_t index) const override {
        const auto field = storage_representation().fixed_item(index);
        const auto *bytes = static_cast<const std::byte *>(value_);
        return {field.representation.input_view_prototype(),
                *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept override {
        const auto &storage = *static_cast<const time_series_bundle_storage *>(value_);
        return valid() && storage.change_time == last_updated() && storage.modified.contains(index);
    }

    void for_each_updated(void *context, void (*function)(void *, std::size_t)) const override {
        const auto &storage = *static_cast<const time_series_bundle_storage *>(value_);
        if (!valid() || storage.change_time != last_updated()) {
            return;
        }
        for (const auto index : storage.modified) {
            function(context, index);
        }
    }
};

class RuntimeTimeSeriesBundleOutputView final : public TimeSeriesBundleOutputView {
  public:
    explicit RuntimeTimeSeriesBundleOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesBundleOutputView{representation} {}

    [[nodiscard]] std::size_t size() const noexcept override {
        return static_cast<const TimeSeriesBundleType &>(storage_representation().type()).item_count();
    }

    [[nodiscard]] Item item(std::size_t index) const override {
        const auto &representation = storage_representation();
        const auto field = representation.fixed_item(index);
        auto *bytes = static_cast<std::byte *>(mutable_value());
        const auto output_path = add_output_path(representation, index);
        const auto *prototype = field.representation.output_view_prototype();
        if (prototype == nullptr) {
            throw std::logic_error{"bundle item storage does not provide a mutable view"};
        }
        return {*prototype, *reinterpret_cast<EvaluationTime *>(bytes + field.last_updated_offset),
                bytes + field.value_offset, output_path};
    }

    void invalidate() const override {
        for (auto index = std::size_t{}; index < size(); ++index) {
            const auto child = item(index);
            auto view = util::polymorphic_value<TimeSeriesOutputView>::copy(child.prototype);
            view->bind(child.last_updated, child.value, child.output_path);
            view->invalidate();
            view->release_output_path();
        }
    }
};

class RuntimeTimeSeriesBundleStorageRepresentation final : public TimeSeriesBundleStorageRepresentationBase {
  public:
    RuntimeTimeSeriesBundleStorageRepresentation(
        const TimeSeriesBundleType &type, std::span<const TimeSeriesStorageRepresentation *const> items)
        : TimeSeriesBundleStorageRepresentationBase{type, items},
          input_{util::polymorphic_value<TimeSeriesInputView>::copy(RuntimeTimeSeriesBundleInputView{*this})},
          output_{util::polymorphic_value<TimeSeriesOutputView>::copy(RuntimeTimeSeriesBundleOutputView{*this})} {}

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override { return input_.get(); }
    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override {
        return &output_.get();
    }

  private:
    util::polymorphic_value<TimeSeriesInputView> input_;
    util::polymorphic_value<TimeSeriesOutputView> output_;
};

[[nodiscard]] inline const TimeSeriesStorageRepresentation &time_series_bundle_storage_representation(
    const TimeSeriesBundleType &type, std::span<const TimeSeriesStorageRepresentation *const> items) {
    struct Entry {
        const TimeSeriesBundleType *type;
        std::vector<const TimeSeriesStorageRepresentation *> items;
        std::unique_ptr<RuntimeTimeSeriesBundleStorageRepresentation> representation;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.type == &type && std::ranges::equal(entry.items, items);
    });
    if (found != entries.end()) {
        return *found->representation;
    }

    auto representation = std::make_unique<RuntimeTimeSeriesBundleStorageRepresentation>(type, items);
    auto &result = *representation;
    entries.push_back({&type, {items.begin(), items.end()}, std::move(representation)});
    return result;
}

template <template <template <typename> typename> typename Schema, std::size_t... Index>
void append_bundle_item_representations(const TimeSeriesBundleType &type,
                                        std::vector<const TimeSeriesStorageRepresentation *> &items,
                                        std::index_sequence<Index...>) {
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        append_bundle_item_representations<Base::template apply>(
            type, items, std::make_index_sequence<direct_bundle_field_count<Base::template apply>>{});
    }
    const auto append = [&]<std::size_t Field>() {
        constexpr auto Item = direct_bundle_field_logical_index<Field, Schema>;
        const auto *representation =
            &CppTimeSeriesStorageRepresentation<direct_bundle_field_time_series<Field, Schema>>::get(
                type.item_type(Item));
        if (Item < items.size()) {
            items[Item] = representation;
        } else {
            items.push_back(representation);
        }
    };
    (append.template operator()<Index>(), ...);
}

template <template <template <typename> typename> typename Schema, auto... Properties>
struct CppTimeSeriesStorageRepresentation<TSB<Schema, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        const auto &bundle = static_cast<const TimeSeriesBundleType &>(type);
        auto items = std::vector<const TimeSeriesStorageRepresentation *>{};
        items.reserve(bundle.item_count());
        append_bundle_item_representations<Schema>(
            bundle, items, std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        return time_series_bundle_storage_representation(bundle, items);
    }
};

} // namespace bg::details
