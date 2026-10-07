#include <bg/graph/details/wiring_record.hpp>

#include <bg/graph/details/wiring_record_overlay.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace bg::details {

WiringPoint WiringRecord::point(const WiringRecord &record) noexcept {
    return WiringPoint{record};
}

void WiringRecord::add_materialisation_option(const TimeSeriesType &, double, Signature,
                                             WiringMaterialisationBuilder) const {
    throw WiringError{"this wiring point does not accept materialisation options"};
}

std::vector<WiringMaterialisationOption> WiringRecord::materialisation_options() const { return {}; }

std::optional<WiringErrorContext> WiringRecord::materialisation_context(const TimeSeriesType &) const {
    return std::nullopt;
}

MaterialisationPreparation WiringRecord::prepare_materialisation(const TimeSeriesType &target_type) const {
    if (!target_type.accepts(type())) {
        throw WiringError{"this wiring point does not provide a compatible semantic form"};
    }
    return {};
}

WiringPoint WiringRecord::expand_materialisation(const TimeSeriesType &, std::span<const WiringArgument>,
                                                 std::span<const TimeSeriesType *const>) const {
    throw WiringError{"this wiring point does not provide materialisation options"};
}

WiringRecordOverlay::WiringRecordOverlay(GraphWiring &wiring) noexcept : wiring_{&wiring} {}

WiringRecordOverlay::~WiringRecordOverlay() = default;

WiringPoint WiringRecordOverlay::add(std::unique_ptr<WiringRecord> record) {
    auto &result = *record;
    result.initialize_overlay(*wiring_);
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

bool WiringPoint::has_default_materialisation() const noexcept {
    return record_ != nullptr && record_->has_default_materialisation();
}

void WiringPoint::add_materialisation_option(const TimeSeriesType &target_type, double cost,
                                            Signature input_signature,
                                            WiringMaterialisationBuilder builder) const {
    if (!has_output()) {
        throw WiringError{"a materialisation option requires a wiring point"};
    }

    record_->wiring().add_materialisation_option(*this, target_type, cost, std::move(input_signature),
                                                 std::move(builder));
}

std::vector<WiringMaterialisationOption> WiringPoint::materialisation_options() const {
    if (!has_output()) {
        throw WiringError{"materialisation options require a wiring point"};
    }

    return record_->wiring().materialisation_options(*this);
}

WiringPoint GraphWiring::materialize(WiringPoint point, const TimeSeriesType &type) {
    using details::WiringRecord;

    if (!point.has_output()) {
        throw WiringError{"wiring-point materialization requires an output"};
    }
    const auto &record = *point.record();
    if (&record.wiring() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto existing = std::ranges::find_if(materialized_wiring_points_, [&](const auto &entry) {
        return entry.record == &record && entry.type == &type;
    });
    if (existing != materialized_wiring_points_.end()) {
        auto result = WiringRecord::point(*existing->result);
        return result;
    }
    ensure_open();

    if (std::ranges::any_of(active_materialisations_, [&](const auto &request) {
            return request.record == &record && request.type == &type;
        })) {
        throw WiringError{"wiring-point materialization contains a dependency cycle"};
    }
    active_materialisations_.push_back({&record, &type});
    auto remove_active_request = util::scope_exit{[&]() noexcept { active_materialisations_.pop_back(); }};

    auto result = record.materialize(type);
    if (!result.has_output()) {
        throw WiringError{"wiring-point materialization returned no point"};
    }
    if (&result.graph() != this) {
        throw WiringError{"wiring-point materialization returned a wiring point from another graph wiring"};
    }
    if (!result.record()->graph_owned()) {
        throw WiringError{"wiring-point materialization returned a temporary wiring record"};
    }
    if (!type.accepts(result.type())) {
        throw WiringError{"wiring-point materialization produced an incompatible type"};
    }

    if (!result.record()->has_wired_output()) {
        result = materialize(result, type);
    }
    if (!result.record()->has_wired_output()) {
        throw WiringError{"wiring-point materialization produced no wired output"};
    }

    if (result.record() != &record) {
        materialized_wiring_points_.push_back({&record, &type, result.record()});
    }
    return result;
}

} // namespace bg
