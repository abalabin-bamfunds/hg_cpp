#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace bg {

class TimeSeriesDictionaryOutputView : public TimeSeriesOutputView {
  public:
    struct Item {
        const TimeSeriesOutputView &prototype;
        EvaluationTime &last_updated;
        void *value;
        void *output_path;
    };

    [[nodiscard]] virtual Item item(const ScalarValueInputView &key) const = 0;
    virtual bool erase(const ScalarValueInputView &key) const = 0;
    virtual void clear() const = 0;

  private:
    void invalidate() const final {}

  protected:
    TimeSeriesDictionaryOutputView() = default;
    explicit TimeSeriesDictionaryOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesOutputView{representation} {}
};

class RuntimeTimeSeriesDictionaryOutputView final : public TimeSeriesDictionaryOutputView {
  public:
    explicit RuntimeTimeSeriesDictionaryOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesDictionaryOutputView{representation} {}

    [[nodiscard]] TimeSeriesDictionaryOutputView::Item item(const ScalarValueInputView &key) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();

        value.reset_change_tracking(context().evaluation_time(), key_representation(), key_layout(),
                                    item_representation(), item_layout(), item_value_offset(), memory);

        const auto result = value.item(key, key_representation(), key_layout(), item_representation(), item_layout(),
                                       item_value_offset(), memory);

        if (result.change == details::time_series_set_change::added) {
            value.key_storage().last_updated = context().evaluation_time();
            publish_item(dictionary_representation(), key_set_item);
        } else if (result.change == details::time_series_set_change::restored) {
            value.mark_item_resurrected(result.position, context().evaluation_time(), memory);
            publish();
        }

        auto *path = add_output_path(dictionary_representation(), dynamic_item(result.position));
        const auto *prototype = item_representation().output_view_prototype();
        if (prototype == nullptr) {
            throw std::logic_error{"time series dictionary item storage does not provide a mutable view"};
        }
        return {*prototype, value.item_last_updated(result.position, item_layout()),
                value.item_value(result.position, item_layout(), item_value_offset()), path};
    }

    bool erase(const ScalarValueInputView &key) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), key_representation(), key_layout(),
                                    item_representation(), item_layout(), item_value_offset(), memory);
        const auto result = value.erase(key, key_representation(), key_layout(), item_representation(), item_layout(),
                                        item_value_offset(), memory);

        if (result.change == details::time_series_set_change::removed) {
            value.key_storage().last_updated = context().evaluation_time();
            publish_item(dictionary_representation(), key_set_item);
        }

        return result.change != details::time_series_set_change::none;
    }

    void clear() const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        const auto should_publish = !valid() || value.key_storage().value.size() != 0;
        value.reset_change_tracking(context().evaluation_time(), key_representation(), key_layout(),
                                    item_representation(), item_layout(), item_value_offset(), memory);
        value.clear(key_representation(), key_layout(), item_representation(), item_layout(), item_value_offset(),
                    memory);

        if (should_publish) {
            value.key_storage().last_updated = context().evaluation_time();
            publish_item(dictionary_representation(), key_set_item);
        }
    }

  private:
    static constexpr auto key_set_item = std::size_t{};

    [[nodiscard]] static std::size_t dynamic_item(std::uint32_t position) noexcept {
        return static_cast<std::size_t>(position) + 1;
    }

    [[nodiscard]] details::time_series_dict_storage &storage() const noexcept {
        return *static_cast<details::time_series_dict_storage *>(mutable_value());
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

    [[nodiscard]] const TimeSeriesStorageRepresentation &dictionary_representation() const noexcept {
        return storage_representation();
    }
};

static_assert(sizeof(TimeSeriesDictionaryOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
