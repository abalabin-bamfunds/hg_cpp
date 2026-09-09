#include <bg/graph/details/wired_output.hpp>

#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include "time_series/ref/ref_node.hpp"

#include <memory>

namespace bg::details {
namespace {

class OutputWiringRecord final : public WiringRecord {
  public:
    explicit OutputWiringRecord(const TimeSeriesType &type) noexcept : WiringRecord{type} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        return materialize_reference(point(*this), requested_type);
    }
};

} // namespace

WiringPoint add_wired_output(GraphWiring &wiring, WiredOutput output) {
    return wiring.register_output(output, std::make_unique<OutputWiringRecord>(*output.type));
}

void set_wired_output_runtime_type(WiringPoint output, const TimeSeriesRuntimeType &runtime_type) {
    const auto &record = wiring_record(output);
    auto &wiring = *record.graph_;
    auto &wired_output = wiring.outputs_[record.output_];
    if (&runtime_type.type() != wired_output.type) {
        throw WiringError{"a wired output runtime type has a different semantic type"};
    }

    wired_output.runtime_type = &runtime_type;
}

WiringPoint wiring_point_for_item(WiringPoint parent, TimeSeriesItemId item, const TimeSeriesType &type) {
    const auto &parent_record = wiring_record(parent);
    auto &wiring = *parent_record.graph_;
    if (parent_record.output_ == WiringRecord::no_output) {
        throw WiringError{"a wiring point must be materialized before selecting an item"};
    }

    const auto parent_index = parent_record.output_;
    if (const auto existing = wiring.find_output_item(parent_index, item)) {
        return *existing;
    }

    const auto node_index = wiring.outputs_[parent_index].node_index;
    return add_wired_output(wiring, {node_index, parent_index, item, &type, nullptr});
}

} // namespace bg::details
