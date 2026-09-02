#pragma once

#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/type/time_series_list_type.hpp>

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

  protected:
    TimeSeriesListInputView() = default;
    explicit TimeSeriesListInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesInputView{representation} {}
};

class RuntimeTimeSeriesListInputView final : public TimeSeriesListInputView {
  public:
    explicit RuntimeTimeSeriesListInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesListInputView{representation} {}

    [[nodiscard]] std::size_t size() const noexcept override {
        return static_cast<const TimeSeriesListType &>(storage_representation().type()).item_count();
    }

    [[nodiscard]] TimeSeriesListInputView::Item item(std::size_t index) const override {
        const auto field = storage_representation().fixed_item(index);
        const auto *bytes = static_cast<const std::byte *>(value_);

        return {field.representation.input_view_prototype(),
                *reinterpret_cast<const EvaluationTime *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept override {
        const auto &storage = *static_cast<const details::time_series_list_storage *>(value_);
        return valid() && storage.change_time == last_updated() && storage.updated.contains(index);
    }

    void for_each_updated(void *context, void (*function)(void *, std::size_t)) const override {
        const auto &storage = *static_cast<const details::time_series_list_storage *>(value_);
        if (!valid() || storage.change_time != last_updated()) {
            return;
        }

        for (const auto index : storage.updated) {
            function(context, index);
        }
    }
};

static_assert(sizeof(TimeSeriesListInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
