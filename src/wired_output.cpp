#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include "time_series/ref/ref_node.hpp"

#include <memory>

namespace bg {
namespace {

class OutputWiringRecord final : public details::WiringRecord {
  public:
    explicit OutputWiringRecord(const TimeSeriesType &type) noexcept : details::WiringRecord{type} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        return details::materialize_reference(point(*this), requested_type);
    }
};

} // namespace

WiringPoint GraphWiring::register_output(WiredOutput output) {
    return register_output(output, std::make_unique<OutputWiringRecord>(*output.type));
}

void GraphWiring::set_output_runtime_type(WiringPoint output, const TimeSeriesRuntimeType &runtime_type) {
    auto &wired_output = outputs_[output_index(output)];
    if (&runtime_type.type() != wired_output.type) {
        throw WiringError{"a wired output runtime type has a different semantic type"};
    }

    wired_output.runtime_type = &runtime_type;
}

WiringPoint GraphWiring::output_item(WiringPoint parent, item_id_t item, const TimeSeriesType &type) {
    const auto parent_index = output_index(parent);
    if (const auto existing = find_output_item(parent_index, item)) {
        return *existing;
    }

    const auto node_index = outputs_[parent_index].node_index;
    return register_output({node_index, parent_index, item, &type, nullptr});
}

} // namespace bg
