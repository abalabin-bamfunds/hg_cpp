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

  protected:
    TimeSeriesDictionaryInputView() = default;
    explicit TimeSeriesDictionaryInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesInputView{representation} {}
};

class RuntimeTimeSeriesDictionaryInputView final : public TimeSeriesDictionaryInputView {
  public:
    explicit RuntimeTimeSeriesDictionaryInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesDictionaryInputView{representation} {}

    [[nodiscard]] std::size_t size() const noexcept override {
        return valid() ? storage().key_storage().value.size() : 0;
    }

    [[nodiscard]] bool contains(const ScalarValueInputView &key) const override {
        return valid() && storage().key_storage().value.contains(key, key_representation(), key_layout());
    }

    [[nodiscard]] TimeSeriesDictionaryInputView::Item at(const ScalarValueInputView &key) const override {
        const auto position = require_position(key);
        return {item_representation().input_view_prototype(), storage().item_last_updated(position, item_layout()),
                storage().item_value(position, item_layout(), item_value_offset())};
    }

    [[nodiscard]] bool item_modified(const ScalarValueInputView &key) const override {
        if (!valid() || storage().change_time() != last_updated()) {
            return false;
        }

        const auto position = storage().key_storage().value.find_position(key, key_representation(), key_layout());
        return position != util::occupancy_index::npos && storage().modified().contains(position);
    }

    void for_each(void *context, EntryFunction function) const override {
        if (!valid()) {
            return;
        }

        const auto &keys = storage().key_storage().value;
        const auto &key_prototype = *key_representation().scalar_input_view_prototype();
        for (const auto position : keys.live()) {
            function(context, {{key_prototype, keys.key(position, key_layout())},
                               {item_representation().input_view_prototype(),
                                storage().item_last_updated(position, item_layout()),
                                storage().item_value(position, item_layout(), item_value_offset())}});
        }
    }

    void for_each_added(void *context, KeyFunction function) const override {
        if (!valid() || storage().change_time() != last_updated()) {
            return;
        }

        const auto &keys = storage().key_storage().value;
        apply(keys.added(), context, function);
    }

    void for_each_removed(void *context, KeyFunction function) const override {
        const auto &keys = storage().key_storage().value;
        if (!valid()) {
            apply(keys.removed(), context, function);
            return;
        }
        if (storage().change_time() == last_updated()) {
            apply(keys.removed(), context, function);
        }
    }

    void for_each_modified(void *context, KeyFunction function) const override {
        if (!valid() || storage().change_time() != last_updated()) {
            return;
        }

        const auto &keys = storage().key_storage().value;
        const auto &prototype = *key_representation().scalar_input_view_prototype();
        for (const auto position : storage().modified()) {
            if (!keys.added().contains(position)) {
                function(context, {prototype, keys.key(position, key_layout())});
            }
        }
    }

  private:
    [[nodiscard]] const details::time_series_dict_storage &storage() const noexcept {
        return *static_cast<const details::time_series_dict_storage *>(value_);
    }

    [[nodiscard]] const ScalarStorageRepresentation &key_representation() const noexcept {
        return *storage_representation().key_storage_representation();
    }

    [[nodiscard]] const TimeSeriesStorageRepresentation &item_representation() const noexcept {
        return *storage_representation().contained_item_representation();
    }

    [[nodiscard]] util::runtime_item_layout key_layout() const noexcept {
        return {key_representation().size(), key_representation().alignment()};
    }

    [[nodiscard]] util::runtime_item_layout item_layout() const noexcept {
        return details::contained_time_series_layout(item_representation());
    }

    [[nodiscard]] std::size_t item_value_offset() const noexcept {
        return details::contained_time_series_value_offset(item_representation());
    }

    [[nodiscard]] std::uint32_t require_position(const ScalarValueInputView &key) const {
        const auto position = storage().key_storage().value.find_position(key, key_representation(), key_layout());
        if (!valid() || position == util::occupancy_index::npos ||
            !storage().key_storage().value.live().contains(position)) {
            throw std::out_of_range{"time series dictionary key was not found"};
        }
        return position;
    }

    void apply(const util::index<> &positions, void *context, KeyFunction function) const {
        const auto &keys = storage().key_storage().value;
        const auto &prototype = *key_representation().scalar_input_view_prototype();
        for (const auto position : positions) {
            function(context, {prototype, keys.key(position, key_layout())});
        }
    }
};

static_assert(sizeof(TimeSeriesDictionaryInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
