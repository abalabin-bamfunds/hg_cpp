#pragma once

#include <bg/time_series/time_series_output_view.hpp>

#include <cstddef>

namespace bg {

class TimeSeriesBundleOutputView : public TimeSeriesOutputView {
  public:
    struct Item {
        const TimeSeriesOutputView &prototype;
        EvaluationTime &last_updated;
        void *value;
        void *output_path;
    };

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual Item item(std::size_t index) const = 0;

  protected:
    TimeSeriesBundleOutputView() = default;
    explicit TimeSeriesBundleOutputView(const TimeSeriesStorageRepresentation &representation) noexcept
        : TimeSeriesOutputView{representation} {}
};

static_assert(sizeof(TimeSeriesBundleOutputView) == sizeof(TimeSeriesOutputView));

} // namespace bg
