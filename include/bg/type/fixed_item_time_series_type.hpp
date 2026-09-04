#pragma once

#include <bg/time_series/item_id.hpp>
#include <bg/type/time_series_type.hpp>

#include <cstddef>

namespace bg {

class FixedItemTimeSeriesType : public TimeSeriesType {
  public:
    [[nodiscard]] virtual std::size_t item_count() const noexcept = 0;
    [[nodiscard]] virtual const TimeSeriesType &item_type(std::size_t index) const = 0;
    [[nodiscard]] virtual TimeSeriesItemId item_id(std::size_t index) const noexcept {
        return static_cast<TimeSeriesItemId>(index);
    }

  protected:
    using TimeSeriesType::TimeSeriesType;
};

} // namespace bg
