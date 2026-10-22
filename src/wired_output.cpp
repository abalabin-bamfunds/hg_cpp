#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/details/wiring_argument_descriptor.hpp>
#include <bg/graph/details/wiring_materialisation.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include "time_series/ref/ref_node.hpp"

#include <algorithm>
#include <memory>
#include <optional>

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
    bool materialisation_ready_impl() const noexcept override { return parent_.record()->materialisation_ready(); }

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
    SelectedMaterialisationFormWiringRecord(WiringPoint source, const TimeSeriesType &type,
                                            const TimeSeriesType *reference_source_target)
        : WiringRecord{type}, source_{source}, target_{&type},
          reference_source_target_{reference_source_target} {}

    bool has_default_materialisation() const noexcept override { return reference_source_target_ != nullptr; }
    std::optional<NodeCallIdentity> node_call_identity() const noexcept override {
        return source_.record()->node_call_identity();
    }
    const details::WiringRecord *source_identity() const noexcept override {
        if (reference_source_target_ != nullptr) {
            return this;
        }
        return source_.record()->source_identity();
    }
    bool materialisation_ready_impl() const noexcept override { return source_.record()->materialisation_ready(); }
    std::vector<WiringMaterialisationOption> materialisation_options() const override {
        if (reference_source_target_ != nullptr) {
            return {};
        }

        auto options = source_.materialisation_options();
        if (source_.has_default_materialisation() && &source_.type() != target_ &&
            std::ranges::none_of(options, [&](const auto &option) { return option.target_type == &source_.type(); })) {
            options.push_back({&source_.type(), 0.0, nullptr});
        }
        return options;
    }

  private:
    const TimeSeriesType *source_target_for(const TimeSeriesType &requested_type) const {
        if (reference_source_target_ != nullptr) {
            return requested_type.accepts(*target_) ? reference_source_target_ : nullptr;
        }

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
        if (reference_source_target_ != nullptr) {
            const auto reference = graph_wiring.materialize(selected, *target_);
            if (requested_type.accepts(reference.type())) {
                return reference;
            }
            return graph_wiring.materialize(reference, requested_type);
        }
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

        if (reference_source_target_ != nullptr) {
            return details::materialize_reference(expanded, *target_);
        }

        if (requested_type.accepts(expanded.type())) {
            return expanded;
        }
        return wiring().materialize(expanded, requested_type);
    }

    WiringPoint source_;
    const TimeSeriesType *target_;
    const TimeSeriesType *reference_source_target_;
};

const TimeSeriesType *reference_source_form(WiringPoint source, const TimeSeriesReferenceType &target) {
    if (source.has_default_materialisation() && target.target_type().accepts(source.type())) {
        return &source.type();
    }

    const auto signature =
        Signature{util::symbol{"reference_target"}, {{util::symbol{"target"}, &target.target_type()}}};
    const auto arguments = std::array{WiringArgument{source}};
    const auto descriptors = std::array{details::WiringArgumentDescriptor{arguments.front()}};
    auto type_map = TypeMap{};
    const auto context = WiringArguments{signature, type_map, descriptors};
    auto preparation = details::WiringCallPreparation{signature, arguments};
    const auto preserve_input = std::array<unsigned char, 1>{};
    auto completed = preparation.complete(preserve_input, context);
    if (completed.calls.empty()) {
        return nullptr;
    }

    struct Candidate {
        const TimeSeriesType *target;
        const TimeSeriesType *resolved_type;
        double cost;
    };
    auto candidates = std::vector<Candidate>{};
    candidates.reserve(completed.calls.size());
    for (auto &call : completed.calls) {
        const auto *selected_target = call.materialisation_target(0);
        if (selected_target == nullptr) {
            selected_target = &source.type();
        }
        const auto *offered_type = call.argument_type(0, arguments.front());
        const auto *resolved_type =
            offered_type == nullptr
                ? nullptr
                : type_cast<TimeSeriesType>(&offered_type->resolve(call.resolution.type_map));
        if (resolved_type == nullptr || type_cast<TimeSeriesReferenceType>(offered_type) != nullptr) {
            continue;
        }
        candidates.push_back({selected_target, resolved_type, call.binding_cost(arguments)});
    }

    if (candidates.empty()) {
        return nullptr;
    }

    const auto lowest_cost = std::ranges::min_element(candidates, {}, &Candidate::cost)->cost;
    const auto selected = std::ranges::find_if(candidates, [lowest_cost](const auto &candidate) {
        return candidate.cost == lowest_cost;
    });
    if (std::ranges::any_of(candidates, [&](const auto &candidate) {
            return candidate.cost == lowest_cost &&
                   (candidate.target != selected->target || candidate.resolved_type != selected->resolved_type);
        })) {
        throw WiringError{"equal-cost reference target forms resolve to distinct concrete forms"};
    }
    return selected->target;
}

} // namespace

WiringPoint GraphWiring::register_output(WiredOutput output) {
    return register_output(output, std::make_unique<OutputWiringRecord>(*output.type));
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
        throw WiringError{"a collection item requires an owning node output"};
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
    const auto exact_form = std::ranges::find(options, &target_type, &WiringMaterialisationOption::target_type);
    auto reference_source_target = static_cast<const TimeSeriesType *>(nullptr);
    auto selected_type = &target_type;
    const auto *target_reference = type_cast<TimeSeriesReferenceType>(&target_type);
    if (exact_form != options.end()) {
        if (target_reference != nullptr) {
            reference_source_target = exact_form->target_type;
        }
    } else {
        if (target_reference == nullptr) {
            throw WiringError{"a selected materialisation form must name an advertised target"};
        }

        const WiringMaterialisationOption *selected_reference_option{};
        for (const auto &option : options) {
            if (type_cast<TimeSeriesReferenceType>(option.target_type) == nullptr ||
                !target_type.accepts(*option.target_type)) {
                continue;
            }
            if (selected_reference_option == nullptr || option.cost < selected_reference_option->cost) {
                selected_reference_option = &option;
            }
        }

        if (selected_reference_option != nullptr) {
            selected_type = selected_reference_option->target_type;
            reference_source_target = selected_reference_option->target_type;
            if (std::ranges::any_of(options, [&](const auto &option) {
                    return option.cost == selected_reference_option->cost &&
                           type_cast<TimeSeriesReferenceType>(option.target_type) != nullptr &&
                           target_type.accepts(*option.target_type) && option.target_type != selected_type;
                })) {
                throw WiringError{"equal-cost reference materialisation options provide distinct forms"};
            }
        } else {
            if (type_cast<TimeSeriesReferenceType>(&point.type()) != nullptr) {
                throw WiringError{"a reference materialisation requires a non-reference source form"};
            }
            reference_source_target = reference_source_form(point, *target_reference);
            if (reference_source_target == nullptr) {
                throw WiringError{"a reference materialisation requires a compatible source form"};
            }
            selected_type = &time_series_reference_type(*reference_source_target, TimeSeriesGuarantee::const_);
            if (!target_type.accepts(*selected_type)) {
                throw WiringError{"the selected reference materialisation does not satisfy the requested type"};
            }
        }
    }

    const auto existing = std::ranges::find_if(selected_materialisation_forms_, [&](const auto &entry) {
        return entry.source == point.record() && entry.type == selected_type;
    });
    if (existing != selected_materialisation_forms_.end()) {
        return details::WiringRecord::point(*existing->result);
    }

    auto result = register_record(std::make_unique<SelectedMaterialisationFormWiringRecord>(
        point, *selected_type, reference_source_target));
    selected_materialisation_forms_.push_back({point.record(), selected_type, result.record()});
    return result;
}

} // namespace bg
