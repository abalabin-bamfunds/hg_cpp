#pragma once

#include <bg/time_series/time_series_input_view.hpp>

#include <cstddef>

namespace bg {

class TimeSeriesBundleInputView : public TimeSeriesInputView {
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
    TimeSeriesBundleInputView() = default;
    explicit TimeSeriesBundleInputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesInputView{representation} {}
};

static_assert(sizeof(TimeSeriesBundleInputView) == sizeof(TimeSeriesInputView));

} // namespace bg
