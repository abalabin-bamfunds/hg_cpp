#include <bg/dispatch.hpp>
#include <bg/dispatch/details/dispatch_selector.hpp>

#include <bg/graph/details/dispatch_cast.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/wiring_materialisation.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/operator.hpp>
#include <bg/switch.hpp>
#include <bg/switch/details/switch_invoke.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>

namespace bg::details {
namespace {

const TimeSeriesValueType *dispatch_value_type(const Type &type) {
    const auto *time_series = type_cast<TimeSeriesType>(&type);
    if (time_series == nullptr) {
        return nullptr;
    }
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(time_series)) {
        time_series = &reference->target_type();
    }
    return type_cast<TimeSeriesValueType>(time_series);
}

const TimeSeriesType &boundary_reference_type(const TimeSeriesType &type) {
    if (type_cast<TimeSeriesReferenceType>(&type) != nullptr) {
        return type;
    }
    return time_series_reference_type(type, TimeSeriesGuarantee::const_);
}

const TimeSeriesType &dispatch_reference_type(const TimeSeriesType &target_form,
                                              const TimeSeriesType &source_type) {
    const auto *source_reference = type_cast<TimeSeriesReferenceType>(&source_type);
    const auto *target_value = dispatch_value_type(target_form);
    const auto *source_value = dispatch_value_type(source_type);
    if (target_value == nullptr || source_value == nullptr) {
        throw WiringError{"dispatch inputs must be value time series"};
    }

    const auto &target = time_series_value_type(target_value->value_type(),
                                                source_value->guarantee().value().value());
    if (source_reference == nullptr) {
        return target;
    }
    return time_series_reference_type(target, source_reference->guarantee().value().value());
}

struct DispatchBranch {
    const WiringTemplate *implementation;
    SignatureResolution resolution;
    std::vector<const TimeSeriesType *> argument_forms;
    std::vector<std::pair<std::size_t, std::size_t>> dispatch_parameters;
};

struct DispatchTypeCombination {
    std::vector<const ScalarType *> types;
    std::vector<const WiringTemplate *> implementations;
    bool static_input_tuple{};
};

struct DispatchInputAlternative {
    const ScalarType *declared_type;
    const ScalarType *resolved_type;
};

struct DispatchImplementationConstraint {
    const WiringTemplate *implementation;
    std::vector<const ScalarType *> declared_types;
    std::vector<const ScalarType *> resolved_types;
};

class DispatchBranchTemplate final : public WiringTemplate {
  public:
    DispatchBranchTemplate(const Signature &dispatch_signature, const TimeSeriesType *output_type,
                           DispatchBranch branch)
        : branch_{std::move(branch)}, dispatch_signature_{&dispatch_signature}, signature_{[&] {
              auto parameters = std::vector<Parameter>{};
              parameters.reserve(dispatch_signature.parameter_count());
              for (auto index = std::size_t{}; index < dispatch_signature.parameter_count(); ++index) {
                  parameters.push_back(dispatch_signature.parameter(index));
              }
              if (output_type == nullptr) {
                  return Signature{util::symbol{"dispatch_branch"}, std::move(parameters)};
              }
              return Signature{util::symbol{"dispatch_branch"}, std::move(parameters), *output_type};
          }()} {}

  private:
    const Signature &default_signature() const final { return signature_; }

    std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        auto adapted = std::vector<WiringArgument>{};
        adapted.reserve(arguments.size());
        for (const auto &argument : arguments) {
            adapted.push_back(WiringArgument::borrow(argument));
        }

        for (const auto &[dispatch_input, parameter_index] : branch_.dispatch_parameters) {
            if (dispatch_input >= adapted.size() || !adapted[dispatch_input].is_time_series()) {
                throw WiringError{"dispatch branch input is not a time series"};
            }

            const auto source = adapted[dispatch_input].time_series();
            auto *target_form = parameter_index < branch_.argument_forms.size()
                                    ? branch_.argument_forms[parameter_index]
                                    : nullptr;
            if (target_form == nullptr && parameter_index < branch_.resolution.signature.parameters.size()) {
                target_form = type_cast<TimeSeriesType>(
                    branch_.resolution.signature.parameters[parameter_index].type);
            }
            if (target_form == nullptr) {
                throw WiringError{"dispatch implementation input is not a time series"};
            }

            const auto &dispatch_form = dispatch_reference_type(*target_form, source.type());
            adapted[dispatch_input].replace_time_series(dispatch_cast(source, dispatch_form));
            adapted[dispatch_input].set_resolution_type(dispatch_form);
        }

        const auto &implementation_signature = branch_.implementation->signature();
        auto committed = normalize_wiring_arguments(implementation_signature, adapted, wiring.allocation(),
                                                     dispatch_signature_);
        bind_materialisation_forms(wiring, *branch_.implementation, committed.arguments, branch_.argument_forms);
        require_concrete_materialisation_forms(wiring, *branch_.implementation, committed.arguments,
                                               branch_.argument_forms);
        auto resolution = SignatureResolution{branch_.resolution.signature, branch_.resolution.type_map.copy(),
                                              branch_.resolution.infer_output};
        const auto result = invoke_resolved(wiring, *branch_.implementation, std::move(resolution),
                                            committed.arguments, committed.descriptors, dispatch_signature_->name());
        if (!result) {
            return std::nullopt;
        }
        if (signature.output == nullptr || !signature.output->accepts(result->type())) {
            throw WiringError{"dispatch implementation output is incompatible with the declared output"};
        }
        const auto *declared_value = dispatch_value_type(*signature.output);
        const auto *result_value = dispatch_value_type(result->type());
        if (declared_value != nullptr && result_value != nullptr) {
            return dispatch_cast(*result, dispatch_reference_type(*signature.output, result->type()));
        }
        if (&signature.output->dereferenced_type() != &result->type().dereferenced_type()) {
            throw WiringError{"dispatch output covariance requires scalar time-series values"};
        }
        return result;
    }

    DispatchBranch branch_;
    const Signature *dispatch_signature_;
    Signature signature_;
};

class DispatchUnmatchedTemplate final : public WiringTemplate {
  public:
    explicit DispatchUnmatchedTemplate(const Signature &dispatch_signature)
        : signature_{[&] {
              auto parameters = std::vector<Parameter>{};
              parameters.reserve(dispatch_signature.parameter_count());
              for (auto index = std::size_t{}; index < dispatch_signature.parameter_count(); ++index) {
                  parameters.push_back(dispatch_signature.parameter(index));
              }
              return Signature{util::symbol{"dispatch_unmatched"}, std::move(parameters)};
          }()} {}

  private:
    const Signature &default_signature() const final { return signature_; }

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument>) const final {
        return std::nullopt;
    }

    Signature signature_;
};

std::vector<WiringArgument> probe_arguments(std::span<const WiringArgument> supplied,
                                            std::span<const std::size_t> dispatch_inputs,
                                            std::span<const ScalarType *const> dispatch_types) {
    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(supplied.size());
    for (auto index = std::size_t{}; index < supplied.size(); ++index) {
        const auto &argument = supplied[index];
        if (!argument.is_time_series()) {
            arguments.push_back(WiringArgument::borrow(argument));
            continue;
        }

        const TimeSeriesType *reference{};
        const auto dispatch_position = std::ranges::find(dispatch_inputs, index);
        if (dispatch_position != dispatch_inputs.end()) {
            const auto dispatch_index = static_cast<std::size_t>(dispatch_position - dispatch_inputs.begin());
            const auto *source_value = dispatch_value_type(argument.type());
            if (source_value == nullptr || dispatch_index >= dispatch_types.size()) {
                throw WiringError{"dispatch inputs must be value time series"};
            }
            const auto &target = time_series_value_type(*dispatch_types[dispatch_index],
                                                        source_value->guarantee().value().value());
            const auto *source_reference = type_cast<TimeSeriesReferenceType>(&argument.type());
            const auto guarantee = source_reference == nullptr
                                       ? TimeSeriesGuarantee::const_
                                       : source_reference->guarantee().value().value();
            reference = &time_series_reference_type(target, guarantee);
        } else {
            reference = &boundary_reference_type(static_cast<const TimeSeriesType &>(argument.type()));
        }

        auto result = WiringArgument{wire_nested_input(static_cast<const TimeSeriesReferenceType &>(*reference))};
        if (argument.name()) {
            result.set_name(*argument.name());
        }
        if (argument.requires_optional()) {
            result.require_optional();
        }
        if (argument.annotations().valid_items) {
            result.require_valid_items();
        }
        arguments.push_back(std::move(result));
    }
    return arguments;
}

std::vector<DispatchInputAlternative> dispatch_input_alternatives(
    const Type &declared_type, const WiringArgument &supplied, const TypeMap &seed_type_map) {
    auto alternatives = std::vector<const Type *>{};
    if (const auto *union_type = dynamic_cast<const TypeVariableType<Type> *>(&declared_type)) {
        for (const auto *alternative : union_type->alternatives()) {
            alternatives.push_back(alternative);
        }
    } else {
        alternatives.push_back(&declared_type);
    }

    auto result = std::vector<DispatchInputAlternative>{};
    for (const auto *alternative : alternatives) {
        const auto *value_type = dispatch_value_type(*alternative);
        if (value_type == nullptr) {
            continue;
        }

        auto scalar_alternatives = std::vector<const ScalarType *>{};
        if (const auto *union_type = dynamic_cast<const TypeVariableType<ScalarType> *>(&value_type->value_type());
            union_type != nullptr && !union_type->alternatives().empty()) {
            for (const auto *scalar_alternative : union_type->alternatives()) {
                scalar_alternatives.push_back(scalar_alternative);
            }
        } else {
            scalar_alternatives.push_back(&value_type->value_type());
        }

        const auto *source_type = dispatch_value_type(supplied.type());
        if (source_type == nullptr) {
            continue;
        }
        for (const auto *declared_scalar : scalar_alternatives) {
            auto type_map = seed_type_map.copy();
            auto *resolved_scalar = declared_scalar;
            if (!resolved_scalar->is_resolved()) {
                try {
                    resolved_scalar = type_cast<ScalarType>(&resolved_scalar->resolve(type_map));
                } catch (const TypeResolutionError &) {
                    if (!declared_scalar->match(source_type->value_type(), type_map)) {
                        continue;
                    }
                    try {
                        resolved_scalar = type_cast<ScalarType>(&declared_scalar->resolve(type_map));
                    } catch (const TypeResolutionError &) {
                        continue;
                    }
                }
            }
            if (resolved_scalar == nullptr || !resolved_scalar->is_resolved() ||
                !resolved_scalar->supports_value_covariance()) {
                continue;
            }
            if (!source_type->value_type().accepts(*resolved_scalar) &&
                !resolved_scalar->accepts(source_type->value_type())) {
                continue;
            }

            result.push_back({declared_scalar, resolved_scalar});
        }
    }
    return result;
}

std::vector<DispatchImplementationConstraint> dispatch_implementation_constraints(
    GraphWiring &wiring, const Signature &prototype, std::span<const std::size_t> dispatch_inputs,
    std::span<const WiringArgument> supplied, std::span<const WiringTemplate *const> implementations,
    const TypeMap &seed_type_map, std::vector<DispatchTypeCombination> &combinations,
    std::vector<WiringCandidateDiagnostic> &failed_probes) {
    auto constraints = std::vector<DispatchImplementationConstraint>{};
    auto static_input_types = std::vector<const ScalarType *>{};
    static_input_types.reserve(dispatch_inputs.size());

    for (auto input = std::size_t{}; input < dispatch_inputs.size(); ++input) {
        const auto supplied_index = dispatch_inputs[input];
        if (supplied_index >= supplied.size() || !supplied[supplied_index].is_time_series()) {
            throw WiringError{"dispatch inputs must be time series"};
        }
        const auto *source_type = dispatch_value_type(supplied[supplied_index].type());
        if (source_type == nullptr || !source_type->value_type().supports_value_covariance()) {
            throw WiringError{"dispatch inputs must support value covariance"};
        }
        static_input_types.push_back(&source_type->value_type());
    }
    combinations.push_back({.types = std::move(static_input_types), .static_input_tuple = true});

    auto records = WiringRecordOverlay{wiring};
    for (const auto *implementation : implementations) {
        records.clear();
        auto normalized = details::NormalizedWiringArguments{};
        try {
            normalized = details::probe_wiring_arguments(implementation->signature(), supplied, records,
                                                         &prototype);
        } catch (const std::exception &error) {
            auto parameters = std::vector<WiringParameterDiagnostic>{};
            const auto &signature = implementation->signature();
            parameters.reserve(signature.parameter_count());
            for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
                const auto &parameter = signature.parameter(index);
                parameters.push_back({parameter.name, parameter.type, parameter.validity, parameter.valid_items});
            }
            failed_probes.push_back({.signature = {signature.name(), std::move(parameters)},
                                     .stage = WiringFailureStage::argument_count,
                                     .message = error.what()});
            continue;
        }

        auto independent_parameters = std::vector<Parameter>{};
        auto independent_arguments = std::vector<WiringArgument>{};
        for (auto parameter_index = std::size_t{}; parameter_index < normalized.arguments.size();
             ++parameter_index) {
            const auto supplied_index = normalized.supplied_argument_indices[parameter_index];
            if (supplied_index && std::ranges::find(dispatch_inputs, *supplied_index) != dispatch_inputs.end()) {
                continue;
            }
            independent_parameters.push_back(implementation->signature().parameter(parameter_index));
            independent_arguments.push_back(WiringArgument::borrow(normalized.arguments[parameter_index]));
        }

        auto type_maps = std::vector<TypeMap>{};
        if (independent_parameters.empty()) {
            type_maps.push_back(seed_type_map.copy());
        } else {
            const auto independent_signature =
                Signature{implementation->signature().name(), std::move(independent_parameters)};
            try {
                auto matches = independent_signature.match_wiring_arguments(independent_arguments, &seed_type_map);
                for (auto &match : matches) {
                    type_maps.push_back(std::move(match.type_map));
                }
            } catch (const TypeResolutionError &error) {
                if (error.code() != TypeResolutionErrorCode::mismatch) {
                    throw;
                }
            }
        }
        if (type_maps.empty()) {
            continue;
        }

        for (const auto &implementation_type_map : type_maps) {
            auto input_alternatives = std::vector<std::vector<DispatchInputAlternative>>{};
            input_alternatives.reserve(dispatch_inputs.size());
            auto complete = true;
            for (const auto supplied_index : dispatch_inputs) {
                const auto parameter = std::ranges::find(normalized.supplied_argument_indices,
                                                         std::optional<std::size_t>{supplied_index});
                if (parameter == normalized.supplied_argument_indices.end()) {
                    complete = false;
                    break;
                }

                const auto parameter_index = static_cast<std::size_t>(
                    parameter - normalized.supplied_argument_indices.begin());
                auto alternatives = dispatch_input_alternatives(
                    *implementation->signature().parameter(parameter_index).type, supplied[supplied_index],
                    implementation_type_map);
                if (alternatives.empty()) {
                    complete = false;
                    break;
                }
                input_alternatives.push_back(std::move(alternatives));
            }
            if (!complete) {
                continue;
            }

            auto cases = std::vector<DispatchImplementationConstraint>{
                {.implementation = implementation, .declared_types = {}, .resolved_types = {}}};
            for (const auto &alternatives : input_alternatives) {
                auto expanded = std::vector<DispatchImplementationConstraint>{};
                expanded.reserve(cases.size() * alternatives.size());
                for (const auto &dispatch_case : cases) {
                    for (const auto &alternative : alternatives) {
                        auto next = dispatch_case;
                        next.declared_types.push_back(alternative.declared_type);
                        next.resolved_types.push_back(alternative.resolved_type);
                        expanded.push_back(std::move(next));
                    }
                }
                cases = std::move(expanded);
            }
            for (auto &dispatch_case : cases) {
                auto combination = std::ranges::find(combinations, dispatch_case.resolved_types,
                                                     &DispatchTypeCombination::types);
                if (combination == combinations.end()) {
                    combinations.push_back({.types = dispatch_case.resolved_types});
                    combination = std::prev(combinations.end());
                }
                if (std::ranges::find(combination->implementations, implementation) ==
                    combination->implementations.end()) {
                    combination->implementations.push_back(implementation);
                }
                constraints.push_back(std::move(dispatch_case));
            }
        }
    }

    return constraints;
}

bool dispatch_constraint_applies(const DispatchImplementationConstraint &constraint,
                                 std::span<const ScalarType *const> dispatch_types) {
    if (constraint.declared_types.size() != dispatch_types.size()) {
        return false;
    }
    for (auto index = std::size_t{}; index < dispatch_types.size(); ++index) {
        if (!constraint.declared_types[index]->accepts(*dispatch_types[index])) {
            return false;
        }
    }
    return true;
}

Signature dispatch_constraint_signature(const DispatchImplementationConstraint &constraint) {
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(constraint.declared_types.size());
    for (const auto *type : constraint.declared_types) {
        parameters.push_back({.type = type});
    }
    return Signature{constraint.implementation->signature().name(), std::move(parameters)};
}

std::vector<const WiringTemplate *> most_specific_dispatch_implementations(
    std::span<const DispatchImplementationConstraint *const> applicable) {
    auto signatures = std::vector<Signature>{};
    signatures.reserve(applicable.size());
    for (const auto *constraint : applicable) {
        signatures.push_back(dispatch_constraint_signature(*constraint));
    }

    const auto compare = [&](std::size_t left, std::size_t right) {
        return compare_parameter_specificity(signatures[left], signatures[right], signatures[left].parameter_count());
    };
    auto most_specific_indices = std::vector<std::size_t>{};
    for (auto candidate = std::size_t{}; candidate < applicable.size(); ++candidate) {
        auto dominated = false;
        for (auto other = std::size_t{}; other < applicable.size(); ++other) {
            if (candidate == other) {
                continue;
            }
            if (compare(candidate, other) == std::partial_ordering::greater) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            most_specific_indices.push_back(candidate);
        }
    }

    for (auto left = std::size_t{}; left < most_specific_indices.size(); ++left) {
        for (auto right = left + 1; right < most_specific_indices.size(); ++right) {
            if (compare(most_specific_indices[left], most_specific_indices[right]) ==
                std::partial_ordering::unordered) {
                throw WiringError{"dispatch input types match incomparable implementation graphs"};
            }
        }
    }

    auto implementations = std::vector<const WiringTemplate *>{};
    for (const auto constraint_index : most_specific_indices) {
        const auto *implementation = applicable[constraint_index]->implementation;
        if (std::ranges::find(implementations, implementation) == implementations.end()) {
            implementations.push_back(implementation);
        }
    }
    return implementations;
}

} // namespace

std::optional<WiringPoint> wire_dispatch(GraphWiring &wiring, const Signature &dispatch_signature,
                                         bool has_output, const TimeSeriesType *output_contract,
                                         const Signature &prototype,
                                         std::span<const WiringArgument> supplied,
                                         std::span<const WiringTemplate *const> implementations,
                                         const Operator &resolver, const TypeMap &seed_type_map,
                                         std::span<const std::size_t> dispatch_inputs) {
    if (implementations.empty()) {
        throw WiringError{"dispatch requires at least one implementation graph"};
    }
    if (dispatch_signature.parameter_count() == 0 || supplied.empty()) {
        throw WiringError{"dispatch requires at least one input"};
    }

    auto selected_inputs = std::vector<std::size_t>{dispatch_inputs.begin(), dispatch_inputs.end()};
    if (selected_inputs.empty()) {
        for (auto index = std::size_t{}; index < supplied.size(); ++index) {
            if (!supplied[index].is_time_series()) {
                continue;
            }
            const auto *input = dispatch_value_type(supplied[index].type());
            if (input != nullptr && input->value_type().supports_value_covariance()) {
                selected_inputs.push_back(index);
            }
        }
    }
    if (selected_inputs.empty()) {
        throw WiringError{"dispatch requires at least one covariance-capable time-series input"};
    }
    auto combinations = std::vector<DispatchTypeCombination>{};
    auto failed_probes = std::vector<WiringCandidateDiagnostic>{};
    const auto implementation_constraints = dispatch_implementation_constraints(
        wiring, prototype, selected_inputs, supplied, implementations, seed_type_map, combinations, failed_probes);

    auto selector_candidates = std::vector<DispatchSelectorCandidate>{};
    auto branches = std::vector<std::unique_ptr<DispatchBranchTemplate>>{};
    auto switch_cases = std::vector<SwitchCaseArgument>{};
    selector_candidates.reserve(combinations.size());
    branches.reserve(combinations.size());
    switch_cases.reserve(combinations.size());

    for (const auto &combination : combinations) {
        const auto &dispatch_types = combination.types;
        auto probe = GraphWiring{wiring};
        auto probe_context = WiringContext{probe};
        auto arguments = probe_arguments(supplied, selected_inputs, dispatch_types);

        auto applicable_constraints = std::vector<const DispatchImplementationConstraint *>{};
        auto applicable_implementations = std::vector<const WiringTemplate *>{};
        for (const auto &constraint : implementation_constraints) {
            if (!dispatch_constraint_applies(constraint, dispatch_types)) {
                continue;
            }
            if (std::ranges::find(applicable_implementations, constraint.implementation) ==
                applicable_implementations.end()) {
                const auto single_implementation =
                    std::array<const WiringTemplate *, 1>{constraint.implementation};
                const auto applicability = resolver.resolve_overloads(probe, arguments, &seed_type_map,
                                                                      single_implementation);
                if (applicability.ambiguous) {
                    throw WiringError{"dispatch input types match an ambiguous implementation graph"};
                }
                if (!applicability.selected) {
                    for (auto &failure : applicability.failures) {
                        failed_probes.push_back(std::move(failure));
                    }
                    continue;
                }
                applicable_implementations.push_back(constraint.implementation);
            }
            applicable_constraints.push_back(&constraint);
        }
        if (applicable_constraints.empty()) {
            continue;
        }

        const auto candidate_implementations = most_specific_dispatch_implementations(applicable_constraints);
        auto resolution = resolver.resolve_overloads(probe, arguments, &seed_type_map,
                                                     candidate_implementations);
        if (resolution.ambiguous) {
            throw WiringError{"dispatch input types match ambiguous implementation graphs"};
        }
        if (!resolution.selected) {
            auto dispatch_type_names = std::string{};
            for (const auto *dispatch_type : dispatch_types) {
                if (!dispatch_type_names.empty()) {
                    dispatch_type_names += ", ";
                }
                dispatch_type->format_name(dispatch_type_names);
            }
            for (auto &failure : resolution.failures) {
                failure.message = "dispatch input candidates [" + dispatch_type_names + "]: " + failure.message;
                failed_probes.push_back(std::move(failure));
            }
            continue;
        }

        if (!combination.static_input_tuple &&
            std::ranges::find(combination.implementations, resolution.selected->implementation) ==
                combination.implementations.end()) {
            continue;
        }

        auto selected = std::move(*resolution.selected);
        const auto *implementation_output = selected.resolution.signature.output;
        if (has_output != (implementation_output != nullptr || selected.resolution.infer_output)) {
            throw WiringError{"dispatch implementations must match the declared output presence"};
        }
        if (selected.resolution.infer_output && output_contract == nullptr) {
            throw WiringError{"dispatch requires a resolved output contract when an implementation infers its output"};
        }
        auto dispatch_parameters = std::vector<std::pair<std::size_t, std::size_t>>{};
        for (const auto dispatch_input : selected_inputs) {
            for (auto parameter = std::size_t{}; parameter < selected.supplied_argument_indices.size(); ++parameter) {
                if (selected.supplied_argument_indices[parameter] == dispatch_input) {
                    dispatch_parameters.emplace_back(dispatch_input, parameter);
                    break;
                }
            }
        }
        auto branch = DispatchBranch{selected.implementation, std::move(selected.resolution),
                                     std::move(selected.argument_forms), std::move(dispatch_parameters)};
        const auto *branch_output = output_contract == nullptr ? implementation_output : output_contract;

        const auto selector_index = selector_candidates.size();
        selector_candidates.push_back({dispatch_types});
        branches.push_back(
            std::make_unique<DispatchBranchTemplate>(dispatch_signature, branch_output, std::move(branch)));
        switch_cases.emplace_back(WiringArgument{ScalarValue<>{static_cast<i64>(selector_index)}}, *branches.back());
    }

    if (selector_candidates.empty()) {
        if (!failed_probes.empty()) {
            throw WiringError{WiringDiagnostic{.operator_name = prototype.name(),
                                                .candidates = std::move(failed_probes)}};
        }
        throw WiringError{"dispatch has no applicable implementation graph"};
    }

    auto selector_inputs = std::vector<WiringArgument>{};
    selector_inputs.reserve(selected_inputs.size());
    for (const auto index : selected_inputs) {
        auto input = WiringArgument::borrow(supplied[index]);
        input.clear_name();
        selector_inputs.push_back(std::move(input));
    }

    auto selector_template = make_dispatch_selector(selector_inputs, selector_candidates);
    const auto &selector_node = wiring.own_node_template(std::move(selector_template));
    const auto selector_output = wiring.invoke_node(selector_node, selector_inputs).output;
    if (!selector_output) {
        throw WiringError{"dispatch selector did not produce an output"};
    }

    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(supplied.size());
    for (const auto &argument : supplied) {
        arguments.push_back(WiringArgument::borrow(argument));
    }
    auto unmatched = DispatchUnmatchedTemplate{dispatch_signature};
    switch_cases.emplace_back(DEFAULT, unmatched);
    auto result = switch_(*selector_output, std::span<const SwitchCaseArgument>{switch_cases},
                          std::span<const WiringArgument>{arguments});
    if (!result.has_output()) {
        return std::nullopt;
    }
    return static_cast<WiringPoint>(result);
}

} // namespace bg::details
