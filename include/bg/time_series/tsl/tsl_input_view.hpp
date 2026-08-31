#pragma once

#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>

#include <cstddef>

namespace bg {

class TimeSeriesListInputView : public TimeSeriesInputView {
  public:
    VISITOR_SUPPORT(final)

    struct Item {
        const TimeSeriesInputView &prototype;
        const EvaluationTime &last_updated;
        const void *value;
    };

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual Item item(std::size_t index) const = 0;
    [[nodiscard]] virtual bool item_updated(std::size_t index) const noexcept = 0;
    virtual void for_each_updated(void *context, void (*function)(void *, std::size_t)) const = 0;
};

template <typename ItemTimeSeries, std::size_t Size>
class TypedTimeSeriesListInputView final : public TimeSeriesListInputView {
    using Storage = details::fixed_time_series_list_storage<ItemTimeSeries, Size>;

  public:
    [[nodiscard]] std::size_t size() const noexcept override { return Size; }

    [[nodiscard]] TimeSeriesListInputView::Item item(std::size_t index) const override {
        const auto &representation = cpp_time_series_storage_representation<ItemTimeSeries>();
        const auto &storage = *static_cast<const Storage *>(value_);
        const auto &item = storage.items.at(index);

        return {representation.input_view_prototype(), item.last_updated, &item.value.value};
    }

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept override {
        return valid() && static_cast<const Storage *>(value_)->changes.contains(index, last_updated());
    }

    void for_each_updated(void *context, void (*function)(void *, std::size_t)) const override {
        const auto &changes = static_cast<const Storage *>(value_)->changes;
        if (!valid() || changes.time() != last_updated()) {
            return;
        }

        for (const auto index : changes) {
            function(context, index);
        }
    }
};

static_assert(sizeof(TimeSeriesListInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
