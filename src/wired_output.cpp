#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include "time_series/ref/ref_node.hpp"

#include <algorithm>
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

class PendingItemWiringRecord final : public details::WiringRecord {
  public:
    PendingItemWiringRecord(const TimeSeriesType &type, WiringPoint parent, item_id_t item)
        : WiringRecord{type}, parent_{parent}, item_{item} {}

    std::optional<NodeCallIdentity> node_call_identity() const noexcept override {
        return parent_.record()->node_call_identity();
    }

  private:
    details::MaterialisationPreparation prepare_materialisation(
        const TimeSeriesType &target_type) const override {
        auto preparation = details::WiringRecord::prepare_materialisation(target_type);
        preparation.dependencies.push_back({parent_, &parent_.type(), {}, {}, std::nullopt, item_});
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto parent = wiring().materialize(parent_, parent_.type());
        const auto item = wiring().output_item(parent, item_, type());
        return wiring().materialize(item, requested_type);
    }

    WiringPoint parent_;
    item_id_t item_;
};

class SelectedMaterialisationFormWiringRecord final : public details::WiringRecord {
  public:
    SelectedMaterialisationFormWiringRecord(WiringPoint source, const TimeSeriesType &type)
        : WiringRecord{type}, source_{source}, target_{&type} {}

    bool has_default_materialisation() const noexcept override { return false; }
    std::optional<NodeCallIdentity> node_call_identity() const noexcept override {
        return source_.record()->node_call_identity();
    }
    const details::WiringRecord *source_identity() const noexcept override {
        return source_.record()->source_identity();
    }
    std::vector<WiringMaterialisationOption> materialisation_options() const override {
        auto options = source_.materialisation_options();
        if (source_.has_default_materialisation() && &source_.type() != target_ &&
            std::ranges::none_of(options, [&](const auto &option) { return option.target_type == &source_.type(); })) {
            options.push_back({&source_.type(), 0.0, nullptr});
        }
        return options;
    }

  private:
    const TimeSeriesType *source_target_for(const TimeSeriesType &requested_type) const {
        const auto options = source_.materialisation_options();
        const auto exact = std::ranges::find(options, &requested_type, &WiringMaterialisationOption::target_type);
        if (exact != options.end()) {
            return exact->target_type;
        }
        if (requested_type.accepts(*target_)) {
            return target_;
        }
        if (source_.has_default_materialisation() && requested_type.accepts(source_.type())) {
            return &source_.type();
        }
        return nullptr;
    }

    details::MaterialisationPreparation prepare_materialisation(const TimeSeriesType &requested_type) const override {
        const auto *source_target = source_target_for(requested_type);
        if (source_target == nullptr) {
            throw WiringError{"a selected materialisation form does not provide a compatible type"};
        }
        auto preparation = details::MaterialisationPreparation{};
        preparation.dependencies.push_back({source_, source_target});
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        auto &graph_wiring = wiring();
        const auto *source_target = source_target_for(requested_type);
        if (source_target == nullptr) {
            throw WiringError{"a selected materialisation form does not provide a compatible type"};
        }
        const auto selected = graph_wiring.materialize(source_, *source_target);
        return graph_wiring.materialize(selected, requested_type);
    }

    WiringPoint expand_materialisation(const TimeSeriesType &requested_type,
                                       std::span<const WiringArgument>,
                                       std::span<const TimeSeriesType *const>) const override {
        const auto *source_target = source_target_for(requested_type);
        if (source_target == nullptr) {
            throw WiringError{"a selected materialisation form does not provide a compatible type"};
        }

        auto expanded = WiringPoint{};
        const auto options = source_.materialisation_options();
        const auto has_option = std::ranges::any_of(options, [&](const auto &option) {
            return option.target_type == source_target;
        });
        if (has_option) {
            expanded = wiring().expand_materialisation(source_, *source_target);
        } else if (source_.has_default_materialisation() && source_target == &source_.type()) {
            expanded = source_;
        } else {
            throw WiringError{"a selected materialisation form does not provide its prepared source form"};
        }

        if (requested_type.accepts(expanded.type())) {
            return expanded;
        }
        return wiring().materialize(expanded, requested_type);
    }

    WiringPoint source_;
    const TimeSeriesType *target_;
};

class RuntimeInputWiringRecord final : public details::WiringRecord {
  public:
    explicit RuntimeInputWiringRecord(const TimeSeriesType &type) noexcept : details::WiringRecord{type} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        if (type_cast<TimeSeriesReferenceType>(&type()) == nullptr &&
            (type_cast<TimeSeriesReferenceType>(&requested_type) != nullptr ||
             requested_type.requires_reference_representation_for(type()))) {
            throw WiringError{"a runtime input cannot create an owning reference output"};
        }
        return details::materialize_reference(point(*this), requested_type);
    }
};

} // namespace

WiringPoint GraphWiring::register_output(WiredOutput output) {
    return register_output(output, std::make_unique<OutputWiringRecord>(*output.type));
}

WiringPoint GraphWiring::runtime_input(const TimeSeriesType &type) {
    return runtime_input(type, resolve_time_series_runtime_type(type));
}

WiringPoint GraphWiring::runtime_input(const TimeSeriesType &type, const TimeSeriesRuntimeType &runtime_type) {
    const auto *reference_type = type_cast<TimeSeriesReferenceType>(&type);
    const auto is_value_series = type_cast<TimeSeriesValueType>(&type) != nullptr;
    const auto is_reference_to_value_series =
        reference_type != nullptr && type_cast<TimeSeriesValueType>(&reference_type->target_type()) != nullptr;
    if (!is_value_series && !is_reference_to_value_series) {
        throw WiringError{"runtime inputs currently support TS<V> and REF<TS<V>> only"};
    }
    if (type.guarantee().value() != TimeSeriesGuarantee::ordinary) {
        throw WiringError{"a rebindable runtime input requires an ordinary guarantee"};
    }
    if (!type.accepts(runtime_type.type())) {
        throw WiringError{"a runtime input representation is incompatible with its time series type"};
    }
    runtime_input_outputs_.reserve(runtime_input_outputs_.size() + 1);
    auto point = register_output({WiredOutput::npos, WiredOutput::npos, 0, &type, &runtime_type},
                                 std::make_unique<RuntimeInputWiringRecord>(type));
    runtime_input_outputs_.push_back(output_index(point));
    return point;
}

std::size_t GraphWiring::runtime_input_index(WiringPoint point) const {
    if (&point.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    for (auto index = std::size_t{}; index < runtime_input_outputs_.size(); ++index) {
        if (output_records_[runtime_input_outputs_[index]] == point.record()) {
            return index;
        }
    }

    throw WiringError{"a WiringPoint is not a runtime input"};
}

WiringPoint GraphWiring::output_item(WiringPoint parent, item_id_t item, const TimeSeriesType &type) {
    if (!parent.record()->has_wired_output()) {
        const auto existing = std::ranges::find_if(item_records_, [&](const auto &entry) {
            return entry.parent == parent.record() && entry.item == item;
        });
        if (existing != item_records_.end()) {
            return details::WiringRecord::point(*existing->result);
        }
        return register_item_record(parent, item,
                                    std::make_unique<PendingItemWiringRecord>(type, parent, item));
    }

    const auto parent_index = output_index(parent);
    auto root = parent_index;
    while (outputs_[root].parent != WiredOutput::npos) {
        root = outputs_[root].parent;
    }
    if (outputs_[root].node_index == WiredOutput::npos) {
        throw WiringError{"runtime inputs do not support collection item selection"};
    }

    if (const auto existing = find_output_item(parent_index, item)) {
        return *existing;
    }

    const auto node_index = outputs_[parent_index].node_index;
    return register_output({node_index, parent_index, item, &type, nullptr});
}

WiringPoint GraphWiring::materialisation_form(WiringPoint point, const TimeSeriesType &target_type) {
    if (!point.has_output() || &point.graph() != this || !point.record()->graph_owned()) {
        throw WiringError{"a selected materialisation form requires a graph-owned point"};
    }
    const auto options = point.materialisation_options();
    if (std::ranges::none_of(options, [&](const auto &option) { return option.target_type == &target_type; })) {
        throw WiringError{"a selected materialisation form must name an advertised target"};
    }

    const auto existing = std::ranges::find_if(selected_materialisation_forms_, [&](const auto &entry) {
        return entry.source == point.record() && entry.type == &target_type;
    });
    if (existing != selected_materialisation_forms_.end()) {
        return details::WiringRecord::point(*existing->result);
    }

    auto result = register_record(std::make_unique<SelectedMaterialisationFormWiringRecord>(point, target_type));
    selected_materialisation_forms_.push_back({point.record(), &target_type, result.record()});
    return result;
}

} // namespace bg
