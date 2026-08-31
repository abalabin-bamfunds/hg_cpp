#pragma once

#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>

#include <cstddef>
#include <cstdint>

namespace bg {

class TimeSeriesDictionaryInputView : public TimeSeriesInputView {
  public:
    VISITOR_SUPPORT(final)

    struct Key {
        const ScalarValueInputView &prototype;
        const void *data;
    };

    struct Item {
        const TimeSeriesInputView &prototype;
        const EvaluationTime &last_updated;
        const void *value;
    };

    struct Entry {
        Key key;
        Item item;
    };

    using EntryFunction = void (*)(void *, Entry);
    using KeyFunction = void (*)(void *, Key);

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual bool contains(const ScalarValueInputView &key) const = 0;
    [[nodiscard]] virtual Item at(const ScalarValueInputView &key) const = 0;
    [[nodiscard]] virtual bool item_modified(const ScalarValueInputView &key) const = 0;
    virtual void for_each(void *context, EntryFunction function) const = 0;
    virtual void for_each_added(void *context, KeyFunction function) const = 0;
    virtual void for_each_removed(void *context, KeyFunction function) const = 0;
    virtual void for_each_modified(void *context, KeyFunction function) const = 0;
};

template <typename K, typename ItemTimeSeries>
class TypedTimeSeriesDictionaryInputView final : public TimeSeriesDictionaryInputView {
    using Storage = details::time_series_dict_storage<K, ItemTimeSeries>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override {
        return valid() ? storage().key_storage().value.value.size() : 0;
    }

    [[nodiscard]] bool contains(const ScalarValueInputView &key) const override {
        return valid() && storage().key_storage().value.value.contains(key);
    }

    [[nodiscard]] TimeSeriesDictionaryInputView::Item at(const ScalarValueInputView &key) const override {
        const auto position = require_position(key);
        const auto &contained = storage().contained(position);
        return {item_representation().input_view_prototype(), contained.last_updated, &contained.value.value};
    }

    [[nodiscard]] bool item_modified(const ScalarValueInputView &key) const override {
        if (!valid() || storage().change_time() != last_updated() || storage().revalidated()) {
            return false;
        }

        const auto position = storage().key_storage().value.value.find_position(key);
        return position != util::occupancy_index::npos && storage().modified().contains(position);
    }

    void for_each(void *context, EntryFunction function) const override {
        if (!valid()) {
            return;
        }

        const auto &keys = storage().key_storage().value.value;
        const auto &key_prototype = *cpp_scalar_storage_representation<K>().scalar_input_view_prototype();
        for (const auto position : keys.live()) {
            const auto &contained = storage().contained(position);
            function(context,
                     {{key_prototype, keys.key(position)},
                      {item_representation().input_view_prototype(), contained.last_updated, &contained.value.value}});
        }
    }

    void for_each_added(void *context, KeyFunction function) const override {
        if (!valid() || storage().change_time() != last_updated()) {
            return;
        }

        const auto &keys = storage().key_storage().value.value;
        apply(storage().revalidated() ? keys.live() : keys.added(), context, function);
    }

    void for_each_removed(void *context, KeyFunction function) const override {
        const auto &keys = storage().key_storage().value.value;
        if (!valid()) {
            apply(keys.live(), context, function);
            return;
        }
        if (storage().change_time() == last_updated() && !storage().revalidated()) {
            apply(keys.removed(), context, function);
        }
    }

    void for_each_modified(void *context, KeyFunction function) const override {
        if (!valid() || storage().change_time() != last_updated() || storage().revalidated()) {
            return;
        }

        const auto &keys = storage().key_storage().value.value;
        const auto &prototype = *cpp_scalar_storage_representation<K>().scalar_input_view_prototype();
        for (const auto position : storage().modified()) {
            if (!keys.added().contains(position)) {
                function(context, {prototype, keys.key(position)});
            }
        }
    }

  private:
    [[nodiscard]] const Storage &storage() const noexcept { return *static_cast<const Storage *>(value_); }

    [[nodiscard]] static const TimeSeriesStorageRepresentation &item_representation() noexcept {
        return cpp_time_series_storage_representation<ItemTimeSeries>();
    }

    [[nodiscard]] std::uint32_t require_position(const ScalarValueInputView &key) const {
        const auto position = storage().key_storage().value.value.find_position(key);
        if (!valid() || position == util::occupancy_index::npos ||
            !storage().key_storage().value.value.live().contains(position)) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        return position;
    }

    void apply(const util::index<> &positions, void *context, KeyFunction function) const {
        const auto &keys = storage().key_storage().value.value;
        const auto &prototype = *cpp_scalar_storage_representation<K>().scalar_input_view_prototype();
        for (const auto position : positions) {
            function(context, {prototype, keys.key(position)});
        }
    }
};

static_assert(sizeof(TimeSeriesDictionaryInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
