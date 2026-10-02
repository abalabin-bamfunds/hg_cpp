#pragma once

#include <bg/time_series/item_id.hpp>

#include <cstddef>
#include <memory>

namespace bg {

class GraphWiring;
class TimeSeriesRuntimeType;
class TimeSeriesType;
class WiringPoint;

namespace details {

class WiringRecordOverlay;

// Retains wiring-time behavior that is not represented by the semantic time series type.
class WiringRecord {
  public:
    static constexpr auto no_output = static_cast<std::size_t>(-1);

    virtual ~WiringRecord() = default;

    GraphWiring &wiring() const noexcept { return *graph_; }
    const TimeSeriesType &type() const noexcept { return *type_; }

  protected:
    explicit WiringRecord(const TimeSeriesType &type, std::size_t output = no_output) noexcept
        : type_{&type}, output_{output} {}

    static WiringPoint point(const WiringRecord &record) noexcept;

  private:
    friend class ::bg::GraphWiring;
    friend class WiringRecordOverlay;

    virtual WiringPoint materialize(const TimeSeriesType &requested_type) const = 0;

    static constexpr auto resolution_only = no_output - 1;
    bool has_wired_output() const noexcept { return output_ != no_output && output_ != resolution_only; }

    GraphWiring *graph_{};
    const TimeSeriesType *type_;
    // A wired output index, no_output, or resolution_only.
    std::size_t output_;
};

WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record);
WiringPoint add_item_wiring_record(WiringPoint parent, item_id_t item,
                                   std::unique_ptr<WiringRecord> record);

} // namespace details
} // namespace bg
