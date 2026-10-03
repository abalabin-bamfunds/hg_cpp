#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/util/tagged_ptr.hpp>

namespace bg {

class TimeSeriesRuntimeType;

// Binds an input view to its runtime type, update time, and storage. It does not own the storage.
class TimeSeriesInputData {
  public:
    TimeSeriesInputData() = default;

    TimeSeriesInputData(const TimeSeriesRuntimeType &runtime_type, const TimeSeriesState &last_updated,
                        const void *data, bool modified = false) noexcept
        : runtime_type_{&runtime_type}, last_updated_{&last_updated, modified}, data_{data} {}

    bool valid() const noexcept { return last_updated_->valid(); }
    bool became_valid() const noexcept { return last_updated_->became_valid(); }
    bool became_invalid() const noexcept { return last_updated_->became_invalid(); }
    bool modified() const noexcept { return last_updated_.tag(); }
    EvaluationTime last_updated() const noexcept { return last_updated_->time(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    const TimeSeriesState &last_updated_ref() const noexcept { return *last_updated_; }
    const void *data() const noexcept { return data_; }

  private:
    const TimeSeriesRuntimeType *runtime_type_{};
    util::tagged_ptr<const TimeSeriesState, 1, bool> last_updated_{&initial_time_series_state};
    const void *data_{};
};

static_assert(sizeof(TimeSeriesInputData) == 3 * sizeof(void *));

} // namespace bg

namespace bg::details {

using ::bg::TimeSeriesInputData;

} // namespace bg::details
