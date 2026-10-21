#include <bg/graph/details/dispatch_cast.hpp>

#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>
#include <optional>

namespace bg::details {
namespace {

struct DispatchCastForm {
    const TimeSeriesType *series;
    const TimeSeriesValueType *value;
    std::optional<TimeSeriesGuarantee> reference_guarantee;
};

std::optional<DispatchCastForm> dispatch_cast_form(const TimeSeriesType &type) {
    if (const auto *value = type_cast<TimeSeriesValueType>(&type)) {
        return DispatchCastForm{value, value, std::nullopt};
    }

    const auto *reference = type_cast<TimeSeriesReferenceType>(&type);
    const auto *value = reference == nullptr ? nullptr : type_cast<TimeSeriesValueType>(&reference->target_type());
    if (reference == nullptr || value == nullptr) {
        return std::nullopt;
    }
    return DispatchCastForm{&reference->target_type(), value, reference->guarantee().value()};
}

bool same_reference_form(const TimeSeriesType &left, const TimeSeriesType &right) noexcept {
    return (type_cast<TimeSeriesReferenceType>(&left) == nullptr) ==
           (type_cast<TimeSeriesReferenceType>(&right) == nullptr);
}

void validate_dispatch_cast(WiringPoint source, const TimeSeriesType &target_type) {
    if (!source.has_output() || !source.record()->graph_owned()) {
        throw WiringError{"a dispatch cast requires a graph-owned wiring point"};
    }
    if (!source.type().is_resolved() || !target_type.is_resolved()) {
        throw WiringError{"a dispatch cast requires resolved time series types"};
    }

    const auto source_form = dispatch_cast_form(source.type());
    const auto target_form = dispatch_cast_form(target_type);
    if (!source_form || !target_form ||
        source_form->reference_guarantee.has_value() != target_form->reference_guarantee.has_value()) {
        throw WiringError{"a dispatch cast requires matching value or reference time series forms"};
    }
    if (source_form->reference_guarantee != target_form->reference_guarantee ||
        source_form->series->guarantee().value() != target_form->series->guarantee().value()) {
        throw WiringError{"a dispatch cast cannot change time series guarantees"};
    }
    const auto source_accepts_target = source.type().accepts(target_type);
    const auto target_accepts_source = target_type.accepts(source.type());
    const auto scalar_accepts_target =
        source_form->value->value_type().accepts(target_form->value->value_type());
    const auto scalar_accepts_source =
        target_form->value->value_type().accepts(source_form->value->value_type());
    if (!source_accepts_target && !target_accepts_source) {
        throw WiringError{"a dispatch cast requires a supported covariant subtype"};
    }
    if (source_accepts_target && !target_accepts_source &&
        (!source_form->value->value_type().supports_value_covariance() || !scalar_accepts_target)) {
        throw WiringError{"a dispatch cast requires a supported covariant subtype"};
    }
    if (target_accepts_source && !source_accepts_target &&
        (!target_form->value->value_type().supports_value_covariance() || !scalar_accepts_source)) {
        throw WiringError{"a dispatch cast requires a compatible supertype"};
    }
}

class DispatchCastWiringRecord final : public WiringRecord {
  public:
    DispatchCastWiringRecord(WiringPoint source, const TimeSeriesType &target_type)
        : WiringRecord{target_type}, source_{source} {}

    std::optional<NodeCallIdentity> node_call_identity() const noexcept override {
        return source_.record()->node_call_identity();
    }

    const WiringRecord *source_identity() const noexcept override {
        return source_.record()->source_identity();
    }

    bool materialisation_ready_impl() const noexcept override {
        return source_.record()->materialisation_ready();
    }

    std::optional<WiredInput> input_binding(const TimeSeriesType &requested_type) const noexcept override {
        if (!requested_type.accepts(type())) {
            return std::nullopt;
        }

        const auto &binding = type_cast<TimeSeriesReferenceType>(&requested_type) == nullptr
                                  ? value_binding_
                                  : reference_binding_;
        return binding;
    }

  private:
    MaterialisationPreparation prepare_materialisation(const TimeSeriesType &requested_type) const override {
        auto preparation = WiringRecord::prepare_materialisation(requested_type);
        const auto &source_type = source_input_type(requested_type);
        preparation.dependencies.push_back({source_, &source_type});
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        if (!requested_type.accepts(type())) {
            throw WiringError{"a dispatch cast does not provide the requested time series type"};
        }

        auto &binding = type_cast<TimeSeriesReferenceType>(&requested_type) == nullptr
                            ? value_binding_
                            : reference_binding_;
        if (!binding) {
            binding = wiring().wire_input(source_, source_input_type(requested_type));
        }
        return point(*this);
    }

    const TimeSeriesType &source_input_type(const TimeSeriesType &requested_type) const {
        const auto source_form = dispatch_cast_form(source_.type()).value();
        const auto *requested_reference = type_cast<TimeSeriesReferenceType>(&requested_type);
        if (requested_reference == nullptr) {
            return *source_form.series;
        }

        const auto guarantee = source_form.reference_guarantee.value_or(
            requested_reference->guarantee().value().value());
        return time_series_reference_type(*source_form.series, guarantee);
    }

    WiringPoint source_;
    mutable std::optional<WiredInput> value_binding_;
    mutable std::optional<WiredInput> reference_binding_;
};

} // namespace

WiringPoint dispatch_cast(WiringPoint source, const TimeSeriesType &target_type) {
    validate_dispatch_cast(source, target_type);
    if (&source.type() == &target_type) {
        return source;
    }
    return source.record()->wiring().register_record(std::make_unique<DispatchCastWiringRecord>(source, target_type));
}

} // namespace bg::details
