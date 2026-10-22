#include <bg/dispatch.hpp>
#include <bg/dispatch/details/dispatch_selector.hpp>

#include <bg/graph/details/dispatch_cast.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
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
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
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
    std::vector<std::optional<std::size_t>> supplied_argument_indices;
    std::vector<std::pair<std::size_t, std::size_t>> dispatch_parameters;
};

struct DispatchTypeCombination {
    std::vector<const ScalarType *> types;
    std::vector<const WiringTemplate *> declarations;
};

class DispatchBranchTemplate final : public WiringTemplate {
  public:
    DispatchBranchTemplate(const Signature &dispatch_signature, const TimeSeriesType *output_type,
                           DispatchBranch branch)
        : branch_{std::move(branch)}, signature_{[&] {
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

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature signature, const WiringArguments &,
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
        auto implementation_arguments = std::vector<WiringArgument>{};
        implementation_arguments.reserve(adapted.size());
        auto used_arguments = std::vector<bool>(adapted.size());
        const auto first_variadic = implementation_signature.first_variadic_parameter().value_or(
            implementation_signature.parameter_count());
        for (auto parameter_index = std::size_t{};
             parameter_index < std::min(first_variadic, branch_.supplied_argument_indices.size()); ++parameter_index) {
            const auto supplied_index = branch_.supplied_argument_indices[parameter_index];
            if (!supplied_index) {
                continue;
            }
            if (*supplied_index >= adapted.size()) {
                throw WiringError{"dispatch implementation argument index is out of range"};
            }

            auto argument = WiringArgument::borrow(adapted[*supplied_index]);
            argument.set_name(implementation_signature.parameter(parameter_index).name);
            argument.clear_variadic_capture_mode();
            argument.match_fixed_parameter_by_name();
            implementation_arguments.push_back(std::move(argument));
            used_arguments[*supplied_index] = true;
        }

        for (auto supplied_index = std::size_t{}; supplied_index < adapted.size(); ++supplied_index) {
            if (used_arguments[supplied_index]) {
                continue;
            }

            const auto mode = adapted[supplied_index].name() ? VariadicMode::named : VariadicMode::positional;
            const auto parameter_index = implementation_signature.variadic_parameter(mode);
            if (!parameter_index) {
                throw WiringError{"dispatch implementation does not accept an unmatched argument"};
            }

            auto argument = WiringArgument::borrow(adapted[supplied_index]);
            const auto &parameter = implementation_signature.parameter(*parameter_index);
            argument.set_variadic_name(parameter.name, mode);
            implementation_arguments.push_back(std::move(argument));
        }

        const auto result = (*branch_.implementation)(branch_.resolution.type_map,
                                                       std::span<const WiringArgument>{implementation_arguments});
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

std::vector<DispatchTypeCombination> dispatch_type_combinations(
    GraphWiring &wiring, const Signature &prototype, std::span<const std::size_t> dispatch_inputs,
    std::span<const WiringArgument> supplied, std::span<const WiringTemplate *const> implementations,
    const TypeMap &seed_type_map) {
    auto static_types = std::vector<const ScalarType *>{};
    static_types.reserve(dispatch_inputs.size());

    for (const auto dispatch_input : dispatch_inputs) {
        if (dispatch_input >= supplied.size() || !supplied[dispatch_input].is_time_series()) {
            throw WiringError{"dispatch inputs must be time series"};
        }
        const auto *input = dispatch_value_type(supplied[dispatch_input].type());
        if (input == nullptr || !input->value_type().supports_value_covariance()) {
            throw WiringError{"dispatch inputs must support value covariance"};
        }

        static_types.push_back(&input->value_type());
    }

    // Runtime dispatch needs the declared input tuple and the tuples named by implementations. Expanding each
    // input's hierarchy independently would create combinations that no implementation can select.
    auto combinations = std::vector<DispatchTypeCombination>{{static_types, {}}};
    auto records = WiringRecordOverlay{wiring};
    for (const auto *implementation : implementations) {
        records.clear();
        auto normalized = details::NormalizedWiringArguments{};
        try {
            normalized = details::probe_wiring_arguments(implementation->signature(), supplied, records,
                                                         &prototype);
        } catch (const std::exception &) {
            continue;
        }

        auto types = static_types;
        for (auto input = std::size_t{}; input < dispatch_inputs.size(); ++input) {
            const auto supplied_index = dispatch_inputs[input];
            const auto parameter = std::ranges::find(normalized.supplied_argument_indices,
                                                     std::optional<std::size_t>{supplied_index});
            if (parameter == normalized.supplied_argument_indices.end()) {
                continue;
            }

            const auto parameter_index = static_cast<std::size_t>(
                parameter - normalized.supplied_argument_indices.begin());
            const auto &declared_type = *implementation->signature().parameter(parameter_index).type;
            auto *implementation_input = dispatch_value_type(declared_type);
            if (implementation_input == nullptr) {
                continue;
            }

            auto *candidate_type = &implementation_input->value_type();
            if (!candidate_type->is_resolved()) {
                try {
                    candidate_type = type_cast<ScalarType>(&candidate_type->resolve(seed_type_map));
                } catch (const TypeResolutionError &) {
                    continue;
                }
            }
            if (candidate_type == nullptr || !candidate_type->is_resolved()) {
                continue;
            }

            if (!candidate_type->supports_value_covariance() ||
                (!candidate_type->accepts(*static_types[input]) && !static_types[input]->accepts(*candidate_type))) {
                continue;
            }
            types[input] = candidate_type;
        }

        const auto existing = std::ranges::find(combinations, types, &DispatchTypeCombination::types);
        if (existing == combinations.end()) {
            combinations.push_back({std::move(types), {implementation}});
        } else if (!existing->declarations.empty() &&
                   std::ranges::find(existing->declarations, implementation) == existing->declarations.end()) {
            existing->declarations.push_back(implementation);
        }
    }
    return combinations;
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
    const auto combinations = dispatch_type_combinations(wiring, prototype, selected_inputs, supplied,
                                                         implementations, seed_type_map);

    auto selector_candidates = std::vector<DispatchSelectorCandidate>{};
    auto branches = std::vector<std::unique_ptr<DispatchBranchTemplate>>{};
    auto switch_cases = std::vector<SwitchCaseArgument>{};
    auto failed_probes = std::vector<WiringCandidateDiagnostic>{};
    selector_candidates.reserve(combinations.size());
    branches.reserve(combinations.size());
    switch_cases.reserve(combinations.size());

    auto candidate_implementations = std::vector<const WiringTemplate *>{implementations.begin(), implementations.end()};
    for (const auto &combination : combinations) {
        const auto &dispatch_types = combination.types;
        auto probe = GraphWiring{wiring};
        auto probe_context = WiringContext{probe};
        auto arguments = probe_arguments(supplied, selected_inputs, dispatch_types);
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

        auto selected = std::move(*resolution.selected);
        if (!combination.declarations.empty() &&
            std::ranges::find(combination.declarations, selected.implementation) == combination.declarations.end()) {
            continue;
        }
        const auto *implementation_output = selected.resolution.signature.output;
        if (has_output != (implementation_output != nullptr)) {
            throw WiringError{"dispatch implementations must match the declared output presence"};
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
                                     std::move(selected.argument_forms),
                                     std::move(selected.supplied_argument_indices),
                                     std::move(dispatch_parameters)};
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
