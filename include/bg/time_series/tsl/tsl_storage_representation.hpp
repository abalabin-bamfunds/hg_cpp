#pragma once

#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_input_view.hpp>
#include <bg/time_series/tsl/tsl_output_view.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg::details {

template <typename TimeSeries>
[[nodiscard]] const TimeSeriesType &concrete_time_series_type(TimeSeriesGuarantee inherited) {
    const auto guarantee = TimeSeries::has_explicit_guarantee ? TimeSeries::guarantee : inherited;

    if constexpr (is_time_series_reference<TimeSeries>) {
        using ReferenceTraits = time_series_reference<TimeSeries>;
        const auto &target =
            concrete_time_series_type<typename ReferenceTraits::target_type>(TimeSeriesGuarantee::ordinary);
        return time_series_reference_type(target, guarantee);
    } else if constexpr (is_time_series_dict<TimeSeries>) {
        using DictTraits = time_series_dict<TimeSeries>;
        const auto &key = cpp_scalar_type<typename DictTraits::key_type>();
        const auto &item = concrete_time_series_type<typename DictTraits::item_type>(guarantee);
        return time_series_dictionary_type(key, item, guarantee);
    } else if constexpr (is_time_series_set<TimeSeries>) {
        using SetTraits = time_series_set<TimeSeries>;
        return time_series_set_type(cpp_scalar_type<typename SetTraits::item_type>(), guarantee);
    } else if constexpr (is_time_series_list<TimeSeries>) {
        using ListTraits = time_series_list<TimeSeries>;
        const auto &item = concrete_time_series_type<typename ListTraits::item_type>(guarantee);
        return time_series_list_type(item, ListTraits::size, guarantee);
    } else {
        return time_series_value_type(cpp_scalar_type<typename TimeSeries::value_type>(), guarantee);
    }
}

template <typename TimeSeries> struct CppTimeSeriesStorageRepresentation;

template <typename T, auto... Properties> struct CppTimeSeriesStorageRepresentation<TS<T, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) noexcept {
        return time_series_value_storage_representation(type, cpp_scalar_storage_representation<T>());
    }
};

template <typename Target, auto... Properties> struct CppTimeSeriesStorageRepresentation<REF<Target, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) noexcept {
        const auto &reference = static_cast<const TimeSeriesReferenceType &>(type);
        return time_series_reference_storage_representation(
            reference, CppTimeSeriesStorageRepresentation<Target>::get(reference.target_type()));
    }
};

template <typename TimeSeries> class CppFixedTimeSeriesListStorageRepresentation;

template <typename Item, std::size_t Size, auto... Properties>
class CppFixedTimeSeriesListStorageRepresentation<TSL<Item, Size, Properties...>> final
    : public TimeSeriesStorageRepresentation {
    using Storage = fixed_time_series_list_storage<Item, Size>;
    using Contained = contained_time_series_storage<Item>;

  public:
    explicit CppFixedTimeSeriesListStorageRepresentation(const TimeSeriesListType &type) noexcept
        : type_{&type}, item_{&CppTimeSeriesStorageRepresentation<Item>::get(type.item_type())} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(Storage); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(Storage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return item_->uses_variable_memory(); }
    [[nodiscard]] bool requires_destruction() const noexcept override { return true; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override {
        return item_->supports_byte_copy_initialization();
    }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesInputView>::template make<TypedTimeSeriesListInputView<Item, Size>>();
        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesOutputView &output_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesOutputView>::template make<TypedTimeSeriesListOutputView<Item, Size>>();
        return prototype.get();
    }

    [[nodiscard]] FixedItem fixed_item(std::size_t index) const override {
        if (index >= Size) {
            throw std::out_of_range{"time series list item index is out of range"};
        }

        const auto item = offsetof(Storage, items) + index * sizeof(Contained);
        return {*item_, item + offsetof(Contained, last_updated), item + offsetof(Contained, value)};
    }

    [[nodiscard]] bool mark_item_modified(void *value, std::size_t item, EvaluationTime evaluation_time,
                                          EvaluationTime previous_last_updated,
                                          std::pmr::memory_resource &) const noexcept override {
        return static_cast<Storage *>(value)->changes.mark(item, evaluation_time, previous_last_updated);
    }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        return static_cast<const Storage *>(value)->valid();
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value,
                                EvaluationTime evaluation_time, void *context,
                                ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < Size; ++index) {
                const auto &item = storage.items[index];
                function(context, {index, item_, &MIN_DT, &item.value.value});
            }
            return;
        }
        if (storage.changes.time() != evaluation_time) {
            return;
        }

        for (const auto index : storage.changes) {
            const auto &item = storage.items[index];
            function(context, {index, item_, &item.last_updated, &item.value.value});
        }
    }

    [[nodiscard]] bool finalize_modified_items(EvaluationTime &last_updated, void *value,
                                               EvaluationTime evaluation_time) const noexcept override {
        auto &storage = *static_cast<Storage *>(value);
        if (storage.changes.time() != evaluation_time) {
            return false;
        }

        for (const auto index : storage.changes) {
            auto &item = storage.items[index];
            if (!item_->finalize_modified_items(item.last_updated, &item.value.value, evaluation_time)) {
                static_cast<void>(storage.changes.reset(index));
            }
        }

        if (!storage.changes.any()) {
            last_updated = storage.changes.previous_last_updated();
            return false;
        }
        return true;
    }

    void construct_initial_value(void *destination) const noexcept override {
        auto *storage = std::construct_at(static_cast<Storage *>(destination));
        for (auto &contained : storage->items) {
            item_->construct_initial_value(&contained.value.value);
        }
    }

    void construct_runtime_value(void *destination, std::pmr::memory_resource &memory) const override {
        auto *storage = std::construct_at(static_cast<Storage *>(destination));
        auto constructed = std::size_t{};
        try {
            for (auto &contained : storage->items) {
                item_->construct_runtime_value(&contained.value.value, memory);
                ++constructed;
            }
        } catch (...) {
            while (constructed != 0) {
                --constructed;
                item_->destroy_value(&storage->items[constructed].value.value, memory);
            }
            std::destroy_at(storage);
            throw;
        }
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<Storage *>(value);
        for (auto item = storage->items.rbegin(); item != storage->items.rend(); ++item) {
            item_->destroy_value(&item->value.value, memory);
        }
        std::destroy_at(storage);
    }

  private:
    const TimeSeriesListType *type_;
    const TimeSeriesStorageRepresentation *item_;
};

template <typename Item, std::size_t Size, auto... Properties>
struct CppTimeSeriesStorageRepresentation<TSL<Item, Size, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) noexcept {
        using TimeSeries = TSL<Item, Size, Properties...>;
        using Representation = CppFixedTimeSeriesListStorageRepresentation<TimeSeries>;

        static auto mutex = std::mutex{};
        static auto representations = std::unordered_map<const TimeSeriesType *, std::unique_ptr<Representation>>{};

        const auto lock = std::lock_guard{mutex};
        auto [entry, inserted] = representations.try_emplace(&type);
        if (inserted) {
            entry->second = std::make_unique<Representation>(static_cast<const TimeSeriesListType &>(type));
        }

        return *entry->second;
    }
};

} // namespace bg::details

namespace bg {

template <typename TimeSeries>
const TimeSeriesStorageRepresentation &cpp_time_series_storage_representation() noexcept {
    const auto &type = details::concrete_time_series_type<TimeSeries>(TimeSeriesGuarantee::ordinary);
    return details::CppTimeSeriesStorageRepresentation<TimeSeries>::get(type);
}

} // namespace bg
