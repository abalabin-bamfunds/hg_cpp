#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/tagged_ptr.hpp>

namespace bg {

class TimeSeriesRuntimeType;

namespace details {

class TimeSeriesInputData {
  public:
    TimeSeriesInputData() = default;

    TimeSeriesInputData(const TimeSeriesRuntimeType &runtime_type, const EvaluationTime &last_updated,
                        const void *data, bool modified = false) noexcept
        : runtime_type_{&runtime_type}, last_updated_{&last_updated, modified}, data_{data} {}

    bool valid() const noexcept { return *last_updated_ > MIN_DT; }
    bool modified() const noexcept { return last_updated_.tag(); }
    EvaluationTime last_updated() const noexcept { return *last_updated_; }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    const EvaluationTime &last_updated_ref() const noexcept { return *last_updated_; }
    const void *data() const noexcept { return data_; }

  private:
    const TimeSeriesRuntimeType *runtime_type_{};
    util::tagged_ptr<const EvaluationTime, 1, bool> last_updated_{&MIN_DT};
    const void *data_{};
};

static_assert(sizeof(TimeSeriesInputData) == 3 * sizeof(void *));

} // namespace details
} // namespace bg
