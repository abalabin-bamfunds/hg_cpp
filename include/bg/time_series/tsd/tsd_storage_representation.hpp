#pragma once

#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_input_view.hpp>
#include <bg/time_series/tsd/tsd_output_view.hpp>
#include <bg/time_series/tss/tss_storage_representation.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>

namespace bg::details {

template <typename TimeSeries> class CppTimeSeriesDictionaryStorageRepresentation;

template <typename K, typename Item, auto... Properties>
class CppTimeSeriesDictionaryStorageRepresentation<TSD<K, Item, Properties...>> final
    : public TimeSeriesStorageRepresentation {
    using TimeSeries = TSD<K, Item, Properties...>;
    using Storage = time_series_dict_storage<K, Item>;
    static_assert(std::is_trivially_copyable_v<Storage>);

  public:
    explicit CppTimeSeriesDictionaryStorageRepresentation(const TimeSeriesDictionaryType &type)
        : type_{&type}, keys_{&CppTimeSeriesStorageRepresentation<TSS<K>>::get(type.key_set_type())},
          item_{&CppTimeSeriesStorageRepresentation<Item>::get(type.value_type())} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(Storage); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(Storage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return true; }
    [[nodiscard]] bool requires_finalization() const noexcept override {
        return keys_->requires_finalization() || item_->requires_finalization();
    }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesInputView>::template make<TypedTimeSeriesDictionaryInputView<K, Item>>();
        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override {
        static const auto prototype = util::polymorphic_value<TimeSeriesOutputView>::template make<
            TypedTimeSeriesDictionaryOutputView<K, Item>>();
        return &prototype.get();
    }

    [[nodiscard]] FixedItem fixed_item(std::size_t index) const override {
        if (index != 0) {
            throw std::out_of_range{"time series dictionary fixed item index is out of range"};
        }

        return {*keys_, Storage::key_last_updated_offset(), Storage::key_value_offset()};
    }

    [[nodiscard]] bool mark_item_modified(void *value, std::size_t item, EvaluationTime evaluation_time, EvaluationTime,
                                          std::pmr::memory_resource &memory) const noexcept override {
        if (item == 0) {
            return true;
        }
        return static_cast<Storage *>(value)->mark_item_modified(static_cast<std::uint32_t>(item - 1), evaluation_time,
                                                                 memory);
    }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        return static_cast<const Storage *>(value)->key_storage().last_updated > MIN_DT;
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        const auto &keys = storage.key_storage().value.value;

        if (last_updated <= MIN_DT) {
            function(context, {0, keys_, &MIN_DT, &storage.key_storage().value.value});
            for (const auto position : keys.removed()) {
                const auto &item = storage.contained(position);
                function(context, {static_cast<std::size_t>(position) + 1, item_, &MIN_DT, &item.value.value});
            }
            return;
        }
        if (storage.change_time() != evaluation_time) {
            return;
        }

        if (storage.key_storage().last_updated == evaluation_time) {
            function(context, {0, keys_, &storage.key_storage().last_updated, &storage.key_storage().value.value});
        }

        for (const auto position : keys.added()) {
            const auto &item = storage.contained(position);
            function(context, {static_cast<std::size_t>(position) + 1, item_, &item.last_updated, &item.value.value});
        }
        for (const auto position : storage.modified()) {
            if (keys.added().contains(position)) {
                continue;
            }
            const auto &item = storage.contained(position);
            function(context, {static_cast<std::size_t>(position) + 1, item_, &item.last_updated, &item.value.value});
        }
        for (const auto position : keys.removed()) {
            const auto &item = storage.contained(position);
            function(context, {static_cast<std::size_t>(position) + 1, item_, &MIN_DT, &item.value.value});
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        auto *storage = std::construct_at(static_cast<Storage *>(destination));
        std::construct_at(&storage->key_storage().value.value);
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<Storage *>(value);
        storage->release(*item_, memory);
        std::destroy_at(&storage->key_storage().value.value);
        std::destroy_at(storage);
    }

  private:
    const TimeSeriesDictionaryType *type_;
    const TimeSeriesStorageRepresentation *keys_;
    const TimeSeriesStorageRepresentation *item_;
};

template <typename K, typename Item, auto... Properties>
struct CppTimeSeriesStorageRepresentation<TSD<K, Item, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        using TimeSeries = TSD<K, Item, Properties...>;
        using Representation = CppTimeSeriesDictionaryStorageRepresentation<TimeSeries>;

        static auto mutex = std::mutex{};
        static auto representations = std::unordered_map<const TimeSeriesType *, std::unique_ptr<Representation>>{};

        const auto lock = std::lock_guard{mutex};
        auto [entry, inserted] = representations.try_emplace(&type);
        if (inserted) {
            entry->second = std::make_unique<Representation>(static_cast<const TimeSeriesDictionaryType &>(type));
        }
        return *entry->second;
    }
};

} // namespace bg::details
