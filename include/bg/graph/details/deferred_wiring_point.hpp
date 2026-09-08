#pragma once

#include <bg/graph/wiring_point.hpp>

#include <cstddef>
#include <memory>

namespace bg {

class TimeSeriesType;

namespace details {

class DeferredWiringPoint {
  public:
    virtual ~DeferredWiringPoint() = default;

    virtual const TimeSeriesType &type() const noexcept = 0;
    virtual WiringPoint materialize(const TimeSeriesType &target_type) const = 0;
    virtual WiringPoint item(std::size_t index) const = 0;
};

WiringPoint defer_wiring_point(std::unique_ptr<DeferredWiringPoint> point);

} // namespace details
} // namespace bg
