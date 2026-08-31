#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>

#include <cstddef>
#include <cstdint>

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
    [[nodiscard]] virtual bool erase(const ScalarValueInputView &key) const = 0;
};

template <typename K, typename ItemTimeSeries>
class TypedTimeSeriesDictionaryOutputView final : public TimeSeriesDictionaryOutputView {
    using TimeSeries = TSD<K, ItemTimeSeries>;
    using Storage = details::time_series_dict_storage<K, ItemTimeSeries>;

  public:
    [[nodiscard]] TimeSeriesDictionaryOutputView::Item item(const ScalarValueInputView &key) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        
        value.reset_change_tracking(context().evaluation_time(), last_updated(), item_representation(), memory);

        const auto result = value.item(key, item_representation(), memory);

        if (result.change == details::time_series_set_change::added) {
            value.key_storage().last_updated = context().evaluation_time();
            publish_item(dictionary_representation(), key_set_item);
        } else if (result.change == details::time_series_set_change::restored) {
            value.mark_item_resurrected(result.position, context().evaluation_time(), memory);
            publish();
        } else if (result.change == details::time_series_set_change::revalidated) {
            value.key_storage().last_updated = context().evaluation_time();
            publish_item(dictionary_representation(), key_set_item);
        }

        refresh_key_set_last_updated();
        refresh_last_updated();
        auto &contained = value.contained(result.position);
        auto *path = add_output_path(dictionary_representation(), dynamic_item(result.position));
        return {item_representation().output_view_prototype(), contained.last_updated, &contained.value.value,
                path};
    }

    [[nodiscard]] bool erase(const ScalarValueInputView &key) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), last_updated(), item_representation(), memory);
        const auto result = value.erase(key, memory);

        if (result.change == details::time_series_set_change::removed) {
            value.key_storage().last_updated = context().evaluation_time();
            publish_item(dictionary_representation(), key_set_item);
        }

        refresh_key_set_last_updated();
        refresh_last_updated();
        return result.change != details::time_series_set_change::none;
    }

    void invalidate() const override {
        if (!valid()) {
            return;
        }

        auto &value = storage();
        value.reset_change_tracking(context().evaluation_time(), last_updated(), item_representation(),
                                    details::current_scalar_memory_resource());
        value.key_storage().last_updated = MIN_DT;
        publish_item(dictionary_representation(), key_set_item);
    }

  private:
    static constexpr auto key_set_item = std::size_t{};

    [[nodiscard]] static std::size_t dynamic_item(std::uint32_t position) noexcept {
        return static_cast<std::size_t>(position) + 1;
    }

    [[nodiscard]] Storage &storage() const noexcept { return *static_cast<Storage *>(mutable_value()); }

    [[nodiscard]] static const TimeSeriesStorageRepresentation &item_representation() noexcept {
        return cpp_time_series_storage_representation<ItemTimeSeries>();
    }

    [[nodiscard]] static const TimeSeriesStorageRepresentation &dictionary_representation() noexcept {
        return cpp_time_series_storage_representation<TimeSeries>();
    }

    void refresh_key_set_last_updated() const noexcept {
        auto &keys = storage().key_storage();
        keys.last_updated =
            keys.value.value.changed() ? context().evaluation_time() : keys.value.value.previous_last_updated();
    }

    void refresh_last_updated() const noexcept {
        auto &value = storage();
        *mutable_last_updated() = value.changed() ? context().evaluation_time() : value.previous_last_updated();
    }
};

static_assert(sizeof(TimeSeriesDictionaryOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
