#pragma once

#include <bg/time_series/item_id.hpp>
#include <bg/type/time_series_type.hpp>

#include <cstddef>

namespace bg {

class FixedItemTimeSeriesType : public TimeSeriesType {
  public:
    virtual std::size_t item_count() const noexcept = 0;
    virtual const TimeSeriesType &item_type(std::size_t index) const = 0;
    virtual item_id_t item_id(std::size_t index) const noexcept { return static_cast<item_id_t>(index); }
    bool requires_reference_representation_for(const TimeSeriesType &supplied) const noexcept override;

  protected:
    using TimeSeriesType::TimeSeriesType;
};

} // namespace bg
