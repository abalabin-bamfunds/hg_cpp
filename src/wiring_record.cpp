#include <bg/graph/details/wiring_record.hpp>

#include <bg/graph/details/wiring_record_overlay.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace bg::details {

WiringPoint WiringRecord::point(const WiringRecord &record) noexcept {
    return WiringPoint{record};
}

WiringRecordOverlay::WiringRecordOverlay(GraphWiring &wiring) noexcept : wiring_{&wiring} {}

WiringRecordOverlay::~WiringRecordOverlay() = default;

WiringPoint WiringRecordOverlay::add(std::unique_ptr<WiringRecord> record) {
    auto &result = *record;
    result.graph_ = wiring_;
    result.output_ = WiringRecord::resolution_only;
    records_.push_back(std::move(record));
    return WiringRecord::point(result);
}

void WiringRecordOverlay::clear() noexcept {
    records_.clear();
}

WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record) {
    return WiringContext::current().wiring().register_record(std::move(record));
}

WiringPoint add_item_wiring_record(WiringPoint parent, item_id_t item,
                                   std::unique_ptr<WiringRecord> record) {
    return parent.record()->wiring().register_item_record(parent, item, std::move(record));
}

} // namespace bg::details

namespace bg {

WiringPoint GraphWiring::materialize(WiringPoint point, const TimeSeriesType &type) {
    using details::WiringRecord;

    const auto &record = *point.record();
    if (record.output_ == WiringRecord::resolution_only) {
        throw WiringError{"a resolution-time wiring point cannot be materialized"};
    }
    if (record.graph_ != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto existing = std::ranges::find_if(materialized_wiring_points_, [&](const auto &entry) {
        return entry.record == &record && entry.type == &type;
    });
    if (existing != materialized_wiring_points_.end()) {
        return WiringRecord::point(*existing->result);
    }

    const auto result = record.materialize(type);
    const auto &result_record = *result.record();
    if (result_record.graph_ != this) {
        throw WiringError{"wiring-point materialization returned a wiring point from another graph wiring"};
    }
    if (!result_record.has_wired_output()) {
        throw WiringError{"wiring-point materialization produced no wired output"};
    }

    if (!type.accepts(result.type())) {
        throw WiringError{"wiring-point materialization produced an incompatible type"};
    }

    if (&result_record != &record) {
        materialized_wiring_points_.push_back({&record, &type, &result_record});
    }
    return result;
}

} // namespace bg
