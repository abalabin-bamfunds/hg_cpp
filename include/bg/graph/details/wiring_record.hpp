#pragma once

#include <bg/time_series/item_id.hpp>

#include <cstddef>
#include <memory>

namespace bg {

class GraphWiring;
class TimeSeriesType;
class WiringPoint;

namespace details {

// Retains wiring-time behavior that is not represented by the semantic time series type.
class WiringRecord {
  public:
    static constexpr auto no_output = static_cast<std::size_t>(-1);

    virtual ~WiringRecord() = default;

    const GraphWiring &graph() const noexcept { return *graph_; }
    const TimeSeriesType &type() const noexcept { return *type_; }

  protected:
    explicit WiringRecord(const TimeSeriesType &type, std::size_t output = no_output) noexcept
        : type_{&type}, output_{output} {}

    static WiringPoint point(const WiringRecord &record) noexcept;

  private:
    friend class ::bg::GraphWiring;
    friend WiringPoint materialize_wiring_point(WiringPoint point, const TimeSeriesType &type);
    friend WiringPoint wiring_point_for_item(WiringPoint parent, TimeSeriesItemId item,
                                             const TimeSeriesType &type);

    virtual WiringPoint materialize(const TimeSeriesType &requested_type) const = 0;

    GraphWiring *graph_{};
    const TimeSeriesType *type_;
    std::size_t output_;
};

WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record);
WiringPoint add_item_wiring_record(WiringPoint parent, TimeSeriesItemId item,
                                   std::unique_ptr<WiringRecord> record);
WiringPoint materialize_wiring_point(WiringPoint point, const TimeSeriesType &type);
const WiringRecord &wiring_record(WiringPoint point) noexcept;

} // namespace details
} // namespace bg
