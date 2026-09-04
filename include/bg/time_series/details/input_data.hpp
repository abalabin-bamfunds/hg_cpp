#pragma once

#include <bg/evaluation_time.hpp>

namespace bg {

class TimeSeriesRuntimeType;

namespace details {

class TimeSeriesInputData {
  public:
    TimeSeriesInputData() = default;

    TimeSeriesInputData(const TimeSeriesRuntimeType &runtime_type, const EvaluationTime &last_updated,
                        const void *data) noexcept
        : runtime_type_{&runtime_type}, last_updated_{&last_updated}, data_{data} {}

    [[nodiscard]] bool valid() const noexcept { return *last_updated_ > MIN_DT; }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return *last_updated_; }
    [[nodiscard]] const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    [[nodiscard]] const EvaluationTime &last_updated_ref() const noexcept { return *last_updated_; }
    [[nodiscard]] const void *data() const noexcept { return data_; }

  private:
    const TimeSeriesRuntimeType *runtime_type_{};
    const EvaluationTime *last_updated_{&MIN_DT};
    const void *data_{};
};

static_assert(sizeof(TimeSeriesInputData) == 3 * sizeof(void *));

} // namespace details
} // namespace bg
