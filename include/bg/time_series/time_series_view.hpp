#pragma once

#include <bg/evaluation_time.hpp>

namespace bg {

class TimeSeriesStorageRepresentation;

class TimeSeriesView {
  public:
    [[nodiscard]] bool valid() const noexcept { return *last_updated_ > MIN_DT; }

    [[nodiscard]] EvaluationTime last_updated() const noexcept { return *last_updated_; }

  protected:
    friend class TimeSeriesReferenceOutputView;
    TimeSeriesView() = default;

    explicit TimeSeriesView(const TimeSeriesStorageRepresentation &representation) noexcept
        : representation_{&representation} {}

    [[nodiscard]] const TimeSeriesStorageRepresentation &storage_representation() const noexcept {
        return *representation_;
    }

    void bind(const EvaluationTime &last_updated, const void *value) noexcept {
        last_updated_ = &last_updated;
        value_ = value;
    }

    const TimeSeriesStorageRepresentation *representation_{};
    const EvaluationTime *last_updated_{&MIN_DT};
    const void *value_{};
};

static_assert(sizeof(TimeSeriesView) == 3 * sizeof(void *));

} // namespace bg
