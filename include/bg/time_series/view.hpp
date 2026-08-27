#pragma once

#include <bg/evaluation_time.hpp>

namespace bg {

class TimeSeriesView {
  public:
    [[nodiscard]] bool valid() const noexcept { return *last_updated_ > MIN_DT; }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return *last_updated_; }

  protected:
    TimeSeriesView() = default;

    TimeSeriesView(const EvaluationTime &last_updated, const void *value) noexcept
        : last_updated_(&last_updated), value_(value) {}

    void bind(const EvaluationTime &last_updated, const void *value) noexcept {
        last_updated_ = &last_updated;
        value_ = value;
    }

    const EvaluationTime *last_updated_{&MIN_DT};
    const void *value_{};
};

static_assert(sizeof(TimeSeriesView) == 2 * sizeof(void *));

} // namespace bg
