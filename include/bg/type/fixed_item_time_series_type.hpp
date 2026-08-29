#pragma once

#include <bg/type/time_series_type.hpp>

#include <cstddef>

namespace bg {

class FixedItemTimeSeriesType : public TimeSeriesType {
  public:
    [[nodiscard]] virtual std::size_t item_count() const noexcept = 0;
    [[nodiscard]] virtual const TimeSeriesType &item_type(std::size_t index) const = 0;

  protected:
    using TimeSeriesType::TimeSeriesType;
};

} // namespace bg
