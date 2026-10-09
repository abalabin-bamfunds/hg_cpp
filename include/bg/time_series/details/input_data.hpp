#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstdint>

namespace bg {

class TimeSeriesRuntimeType;

// Binds an input view to its runtime type, update time, and storage. It does not own the storage.
class TimeSeriesInputData {
  public:
    TimeSeriesInputData() = default;

    TimeSeriesInputData(const TimeSeriesRuntimeType &runtime_type, const TimeSeriesState &last_updated,
                        const void *data, bool modified = false, bool valid_items = false) noexcept
        : runtime_type_{&runtime_type}, last_updated_{&last_updated, flags(modified, valid_items)}, data_{data} {}

    bool valid() const noexcept { return last_updated_->valid(); }
    bool became_valid() const noexcept { return last_updated_->became_valid(); }
    bool became_invalid() const noexcept { return last_updated_->became_invalid(); }
    bool modified() const noexcept { return (last_updated_.raw() & modified_flag) != 0; }
    bool valid_items() const noexcept { return (last_updated_.raw() & valid_items_flag) != 0; }
    void set_valid_items(bool valid_items) noexcept {
        last_updated_.reset_tag(flags(modified(), valid_items));
    }
    EvaluationTime last_updated() const noexcept { return last_updated_->time(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    const TimeSeriesState &last_updated_ref() const noexcept { return *last_updated_; }
    const void *data() const noexcept { return data_; }

  private:
    static constexpr std::uintptr_t modified_flag = 1;
    static constexpr std::uintptr_t valid_items_flag = 2;

    static constexpr std::uint8_t flags(bool modified, bool valid_items) noexcept {
        return static_cast<std::uint8_t>(modified | (valid_items << 1));
    }

    const TimeSeriesRuntimeType *runtime_type_{};
    util::tagged_ptr<const TimeSeriesState, 2, std::uint8_t> last_updated_{&initial_time_series_state};
    const void *data_{};
};

static_assert(sizeof(TimeSeriesInputData) == 3 * sizeof(void *));
static_assert(alignof(TimeSeriesState) >= 4);

} // namespace bg

namespace bg::details {

using ::bg::TimeSeriesInputData;

} // namespace bg::details
