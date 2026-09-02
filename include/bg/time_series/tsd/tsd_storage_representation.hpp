#pragma once

#include <bg/time_series/cpp_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_input_view.hpp>
#include <bg/time_series/tsd/tsd_output_view.hpp>
#include <bg/time_series/tss/tss_storage_representation.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

class TimeSeriesDictionaryStorageRepresentation final : public TimeSeriesStorageRepresentation {
  public:
    TimeSeriesDictionaryStorageRepresentation(const TimeSeriesDictionaryType &type,
                                              const ScalarStorageRepresentation &key,
                                              const TimeSeriesStorageRepresentation &item)
        : type_{&type}, key_{&key},
          keys_{&time_series_set_storage_representation(
              static_cast<const TimeSeriesSetType &>(type.key_set_type()), key)},
          item_{&item},
          key_layout_{key.size(), key.alignment()}, item_layout_{contained_time_series_layout(item)},
          item_value_offset_{contained_time_series_value_offset(item)},
          input_{util::polymorphic_value<TimeSeriesInputView>::copy(RuntimeTimeSeriesDictionaryInputView{*this})},
          output_{util::polymorphic_value<TimeSeriesOutputView>::copy(RuntimeTimeSeriesDictionaryOutputView{*this})} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(time_series_dict_storage); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(time_series_dict_storage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return true; }
    [[nodiscard]] bool requires_finalization() const noexcept override {
        return key_->requires_finalization() || item_->requires_finalization();
    }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }
    [[nodiscard]] const ScalarStorageRepresentation *key_storage_representation() const noexcept override {
        return key_;
    }
    [[nodiscard]] const TimeSeriesStorageRepresentation *contained_item_representation() const noexcept override {
        return item_;
    }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override { return input_.get(); }
    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override {
        return &output_.get();
    }

    [[nodiscard]] FixedItem fixed_item(std::size_t index) const override {
        if (index != 0) {
            throw std::out_of_range{"time series dictionary fixed item index is out of range"};
        }

        return {*keys_, time_series_dict_storage::key_last_updated_offset(),
                time_series_dict_storage::key_value_offset()};
    }

    [[nodiscard]] bool mark_item_modified(void *value, std::size_t item, EvaluationTime evaluation_time,
                                          EvaluationTime, std::pmr::memory_resource &memory) const noexcept override {
        if (item == 0) {
            return true;
        }
        return static_cast<time_series_dict_storage *>(value)->mark_item_modified(
            static_cast<std::uint32_t>(item - 1), evaluation_time, memory);
    }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().last_updated > MIN_DT;
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        const auto &keys = storage.key_storage().value;
        const auto publish_item = [&](std::uint32_t position, const EvaluationTime &item_last_updated) {
            function(context, {static_cast<std::size_t>(position) + 1, item_, &item_last_updated,
                               storage.item_value(position, item_layout_, item_value_offset_)});
        };

        if (last_updated <= MIN_DT) {
            function(context, {0, keys_, &MIN_DT, &storage.key_storage().value});
            for (const auto position : keys.removed()) {
                publish_item(position, MIN_DT);
            }
            return;
        }
        if (storage.change_time() != evaluation_time) {
            return;
        }

        if (storage.key_storage().last_updated == evaluation_time) {
            function(context, {0, keys_, &storage.key_storage().last_updated, &storage.key_storage().value});
        }
        for (const auto position : keys.added()) {
            publish_item(position, storage.item_last_updated(position, item_layout_));
        }
        for (const auto position : storage.modified()) {
            if (!keys.added().contains(position)) {
                publish_item(position, storage.item_last_updated(position, item_layout_));
            }
        }
        for (const auto position : keys.removed()) {
            publish_item(position, MIN_DT);
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_dict_storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<time_series_dict_storage *>(value);
        storage->release(*key_, key_layout_, *item_, item_layout_, item_value_offset_, memory);
        std::destroy_at(storage);
    }

  private:
    const TimeSeriesDictionaryType *type_;
    const ScalarStorageRepresentation *key_;
    const TimeSeriesStorageRepresentation *keys_;
    const TimeSeriesStorageRepresentation *item_;
    util::runtime_item_layout key_layout_;
    util::runtime_item_layout item_layout_;
    std::size_t item_value_offset_;
    util::polymorphic_value<TimeSeriesInputView> input_;
    util::polymorphic_value<TimeSeriesOutputView> output_;
};

[[nodiscard]] inline const TimeSeriesStorageRepresentation &
time_series_dictionary_storage_representation(const TimeSeriesDictionaryType &type,
                                              const ScalarStorageRepresentation &key,
                                              const TimeSeriesStorageRepresentation &item) {
    struct Entry {
        const TimeSeriesDictionaryType *type;
        const ScalarStorageRepresentation *key;
        const TimeSeriesStorageRepresentation *item;
        std::unique_ptr<TimeSeriesDictionaryStorageRepresentation> representation;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.type == &type && entry.key == &key && entry.item == &item;
    });
    if (found != entries.end()) {
        return *found->representation;
    }

    auto representation = std::make_unique<TimeSeriesDictionaryStorageRepresentation>(type, key, item);
    auto &result = *representation;
    entries.push_back({&type, &key, &item, std::move(representation)});
    return result;
}

template <typename K, typename Item, auto... Properties>
struct CppTimeSeriesStorageRepresentation<TSD<K, Item, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        const auto &dictionary = static_cast<const TimeSeriesDictionaryType &>(type);
        return time_series_dictionary_storage_representation(
            dictionary, cpp_scalar_storage_representation<K>(),
            CppTimeSeriesStorageRepresentation<Item>::get(dictionary.value_type()));
    }
};

} // namespace bg::details
