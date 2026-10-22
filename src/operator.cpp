#include <bg/graph/operator.hpp>

#include <bg/graph/context.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/details/wiring_materialisation.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/details/resolver_type_resolution_error.hpp>
#include <bg/type/type_resolution_error.hpp>

#include "operator_resolution_cache.hpp"

#include <algorithm>
#include <compare>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace bg {
namespace {

WiringFailureStage failure_stage(TypeResolutionErrorCode code) noexcept {
    switch (code) {
    case TypeResolutionErrorCode::argument_count:
        return WiringFailureStage::argument_count;
    case TypeResolutionErrorCode::mismatch:
    case TypeResolutionErrorCode::ambiguous_constraint:
        return WiringFailureStage::type_match;
    case TypeResolutionErrorCode::unmapped_variable:
    case TypeResolutionErrorCode::invalid_type_map:
        return WiringFailureStage::resolver;
    }
    return WiringFailureStage::resolver;
}

std::partial_ordering compare_input_requirement_preference(const ResolvedSignature &left,
                                                           const ResolvedSignature &right,
                                                           std::span<const std::optional<std::size_t>>
                                                               left_supplied_indices,
                                                           std::span<const std::optional<std::size_t>>
                                                               right_supplied_indices,
                                                           std::span<const InputValidity> left_arguments,
                                                           std::span<const InputValidity> right_arguments,
                                                           std::span<const unsigned char> left_valid_items,
                                                           std::size_t parameter_count) noexcept {
    auto left_better = false;
    auto right_better = false;
    for (auto index = std::size_t{}; index < parameter_count; ++index) {
        const auto &left_parameter = left.parameters[index];
        const auto right_index = [&]() -> std::optional<std::size_t> {
            if (left_supplied_indices[index] && !left_parameter.context) {
                for (auto right_index = std::size_t{}; right_index < right_supplied_indices.size(); ++right_index) {
                    if (right_supplied_indices[right_index] == left_supplied_indices[index]) {
                        return right_index;
                    }
                }
                return std::nullopt;
            }

            const auto named_parameter = std::ranges::find(right.parameters, left_parameter.name,
                                                            &ResolvedParameter::name);
            if (named_parameter != right.parameters.end()) {
                return static_cast<std::size_t>(named_parameter - right.parameters.begin());
            }
            return index;
        }();
        if (!right_index) {
            continue;
        }
        const auto &right_parameter = right.parameters[*right_index];
        if (left_parameter.type->is_scalar() && right_parameter.type->is_scalar()) {
            const auto left_requires_presence = !left_parameter.accepts_absent_scalar;
            const auto right_requires_presence = !right_parameter.accepts_absent_scalar;
            left_better |= left_requires_presence && !right_requires_presence;
            right_better |= right_requires_presence && !left_requires_presence;
            continue;
        }
        if (!left_parameter.type->is_time_series() || !right_parameter.type->is_time_series()) {
            continue;
        }
        const auto left_supports_valid_items = left_parameter.valid_items;
        const auto right_supports_valid_items = right_parameter.valid_items;
        if (left_valid_items[index]) {
            left_better |= left_supports_valid_items && !right_supports_valid_items;
            right_better |= right_supports_valid_items && !left_supports_valid_items;
        } else {
            left_better |= !left_supports_valid_items && right_supports_valid_items;
            right_better |= !right_supports_valid_items && left_supports_valid_items;
        }
        if (left_arguments[index] == optional || right_arguments[*right_index] == optional) {
            continue;
        }

        const auto left_required = left_parameter.validity != optional;
        const auto right_required = right_parameter.validity != optional;
        left_better |= left_required && !right_required;
        right_better |= right_required && !left_required;
    }

    if (left_better == right_better) {
        return left_better ? std::partial_ordering::unordered : std::partial_ordering::equivalent;
    }
    return left_better ? std::partial_ordering::less : std::partial_ordering::greater;
}

bool preferred(const WiringTemplate &candidate, const ResolvedSignature &candidate_signature,
               const WiringTemplate &other, const ResolvedSignature &other_signature,
               std::span<const Type *const> candidate_arguments, std::span<const Type *const> other_arguments,
               std::span<const std::optional<std::size_t>> candidate_supplied_indices,
               std::span<const std::optional<std::size_t>> other_supplied_indices,
               std::span<const InputValidity> candidate_argument_validity,
               std::span<const InputValidity> other_argument_validity,
               std::span<const unsigned char> candidate_argument_valid_items) {
    const auto candidate_variadic = candidate.signature().has_variadic_parameters();
    const auto other_variadic = other.signature().has_variadic_parameters();
    if (candidate_variadic != other_variadic) {
        return !candidate_variadic;
    }
    if (candidate_variadic) {
        const auto candidate_fixed = *candidate.signature().first_variadic_parameter();
        const auto other_fixed = *other.signature().first_variadic_parameter();
        const auto shared_fixed = std::min(candidate_fixed, other_fixed);
        if (candidate_fixed == other_fixed) {
            const auto parameter_count = candidate.signature().parameter_count();
            const auto specificity = parameter_count == other.signature().parameter_count()
                                        ? details::compare_parameter_specificity(
                                              candidate.signature(), other.signature(), candidate_arguments,
                                              other_arguments, parameter_count)
                                        : std::partial_ordering::unordered;
            if (specificity != std::partial_ordering::equivalent) {
                return specificity == std::partial_ordering::less;
            }
            const auto shared_parameters =
                std::min(candidate_signature.parameters.size(), other_signature.parameters.size());
            return compare_input_requirement_preference(candidate_signature, other_signature,
                                                        candidate_supplied_indices, other_supplied_indices,
                                                        candidate_argument_validity, other_argument_validity,
                                                        candidate_argument_valid_items, shared_parameters) ==
                   std::partial_ordering::less;
        }

        const auto shared_specificity = details::compare_parameter_specificity(
            candidate.signature(), other.signature(), candidate_arguments, other_arguments, shared_fixed);
        const auto prefix_length =
            candidate_fixed > other_fixed ? std::partial_ordering::less : std::partial_ordering::greater;
        const auto preferences_agree =
            shared_specificity == std::partial_ordering::equivalent || shared_specificity == prefix_length;
        return preferences_agree && prefix_length == std::partial_ordering::less;
    }
    const auto shared_parameters = std::min(candidate_signature.parameters.size(), other_signature.parameters.size());
    const auto parameter_count = candidate.signature().parameter_count();
    const auto specificity = parameter_count == other.signature().parameter_count()
                                 ? details::compare_parameter_specificity(candidate.signature(), other.signature(),
                                                                         candidate_arguments, other_arguments,
                                                                         parameter_count)
                                 : std::partial_ordering::unordered;
    if (specificity != std::partial_ordering::equivalent) {
        return specificity == std::partial_ordering::less;
    }
    return compare_input_requirement_preference(candidate_signature, other_signature,
                                                candidate_supplied_indices, other_supplied_indices,
                                                candidate_argument_validity, other_argument_validity,
                                                candidate_argument_valid_items, shared_parameters) ==
           std::partial_ordering::less;
}

std::vector<util::symbol> current_path() {
    auto result = std::vector<util::symbol>{};
    for (auto *path = WiringContext::current().path(); path != nullptr; path = path->parent) {
        result.push_back(path->label);
    }
    std::ranges::reverse(result);
    return result;
}

WiringSignatureDiagnostic diagnostic_signature(const WiringTemplate &implementation) {
    auto parameters = std::vector<WiringParameterDiagnostic>{};
    parameters.reserve(implementation.signature().parameter_count());
    for (auto index = std::size_t{}; index < implementation.signature().parameter_count(); ++index) {
        const auto &parameter = implementation.signature().parameter(index);
        parameters.push_back({parameter.name, parameter.type, parameter.validity, parameter.valid_items});
    }

    return {implementation.signature().name(), std::move(parameters)};
}

WiringCandidateDiagnostic failure(const WiringTemplate &implementation, const TypeResolutionError &error) {
    return {
        .signature = diagnostic_signature(implementation),
        .stage = failure_stage(error.code()),
        .subject = error.subject(),
        .expected = error.expected(),
        .supplied = error.supplied(),
        .message = error.what(),
    };
}

WiringCandidateDiagnostic failure(const WiringTemplate &implementation, WiringFailureStage stage, std::string message) {
    return {
        .signature = diagnostic_signature(implementation),
        .stage = stage,
        .subject = std::nullopt,
        .expected = nullptr,
        .supplied = nullptr,
        .message = std::move(message),
    };
}

WiringCandidateDiagnostic matched_failure(const WiringTemplate &implementation, const ResolvedSignature &resolution,
                                          std::span<const WiringArgumentFormMatch> forms, double cost,
                                          WiringFailureStage stage, std::string message) {
    auto result = failure(implementation, stage, std::move(message));
    result.joint_materialisation_cost = cost;
    result.bindings.reserve(forms.size());
    const auto &signature = implementation.signature();
    for (const auto &form : forms) {
        const auto index = form.argument_index;
        const auto parameter_name = index < signature.parameter_count()
                                        ? signature.parameter(index).name
                                        : util::symbol{std::to_string(index)};
        auto offered_type = std::string{};
        form.offered_type->format_name(offered_type);
        auto accepted_type = std::string{};
        if (index < resolution.parameters.size()) {
            resolution.parameters[index].type->format_name(accepted_type);
        }
        auto materialisation_type = std::string{};
        if (form.materialisation_target != nullptr) {
            form.materialisation_target->format_name(materialisation_type);
        }
        result.bindings.push_back({index, parameter_name, std::move(offered_type), std::move(accepted_type),
                                   std::move(materialisation_type), form.materialisation_cost});
    }
    return result;
}

void require_concrete_materialisation_forms(GraphWiring &wiring, const WiringTemplate &implementation,
                                             std::span<const WiringArgument> arguments,
                                             std::span<const TimeSeriesType *const> selected_forms) {
    const auto &signature = implementation.signature();
    const auto *path = WiringContext::current().path();
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        if (!arguments[index].is_time_series() ||
            implementation.preserves_input_point(index) ||
            selected_forms[index] == nullptr) {
            continue;
        }
        wiring.require_materialisation(
            arguments[index].time_series(), *selected_forms[index],
            {.path = path == nullptr ? std::string{} : path->text(),
             .consumer = signature.name(),
             .parameter = signature.parameter(index).name});
    }
}

WiringDiagnostic diagnostic(const Signature &prototype, std::vector<util::symbol> path,
                            std::span<const WiringArgument> supplied, std::vector<WiringCandidateDiagnostic> candidates,
                            bool ambiguous = false) {
    auto arguments = std::vector<WiringParameterDiagnostic>{};
    arguments.reserve(supplied.size());
    for (auto index = std::size_t{}; index < supplied.size(); ++index) {
        const auto name =
            supplied[index].name().value_or(index < prototype.parameter_count() ? prototype.parameter(index).name
                                                                                : util::symbol{std::to_string(index)});
        const auto validity = supplied[index].requires_optional() ? InputValidity::optional : InputValidity::required;
        auto argument = WiringParameterDiagnostic{name, &supplied[index].type(), validity,
                                                  supplied[index].annotations().valid_items};
        const auto append_form = [&](const Type &type, double cost, bool direct) {
            auto text = std::string{};
            type.format_name(text);
            argument.offered_forms.push_back({std::move(text), cost, direct});
        };
        if (!supplied[index].is_time_series()) {
            append_form(supplied[index].type(), 0.0, true);
        } else {
            const auto point = supplied[index].time_series();
            if (point.has_default_materialisation()) {
                append_form(supplied[index].type(), 0.0, true);
            }
            for (const auto &option : point.materialisation_options()) {
                append_form(*option.target_type, option.cost, false);
            }
        }
        arguments.push_back(std::move(argument));
    }

    return {prototype.name(), std::move(path), std::move(arguments), std::move(candidates), ambiguous};
}

using ArgumentKey = details::OperatorArgumentKey;

std::size_t source_group_index(std::vector<const details::WiringRecord *> &sources,
                               const details::WiringRecord *record) {
    const auto existing = std::ranges::find(sources, record);
    if (existing != sources.end()) {
        return static_cast<std::size_t>(existing - sources.begin());
    }
    sources.push_back(record);
    return sources.size() - 1;
}

bool append_argument_key(std::vector<ArgumentKey> &result,
                         std::vector<const details::WiringRecord *> &sources, const WiringArgument &argument) {
    if (argument.is_graph()) {
        const auto &graph = argument.graph();
        const auto &structures = graph.structures();
        const auto cacheable = std::visit(
            [](const auto &value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, details::GraphWiringStructure>) {
                    return value.cacheable();
                } else {
                    return !value.empty() &&
                           std::ranges::all_of(value, &details::GraphWiringStructure::cacheable);
                }
            },
            structures);
        if (!cacheable) {
            return false;
        }
        result.push_back({.type = nullptr,
                          .graph_structures = structures,
                          .present = true,
                          .name = argument.name(),
                          .variadic_capture_mode = argument.variadic_capture_mode()});
        return true;
    }

    auto options = std::vector<std::pair<const TimeSeriesType *, double>>{};
    auto has_default_materialisation = false;
    auto source_group = static_cast<std::size_t>(-1);
    if (argument.is_time_series()) {
        const auto point = argument.time_series();
        has_default_materialisation = point.has_default_materialisation();
        source_group = source_group_index(sources, point.record()->source_identity());
        for (const auto &option : point.materialisation_options()) {
            options.emplace_back(option.target_type, option.cost);
        }
    }
    result.push_back({.type = &argument.type(),
                      .present = argument.present(),
                      .name = argument.name(),
                      .requires_optional = argument.requires_optional(),
                      .requires_valid_items = argument.annotations().valid_items,
                      .variadic_capture_mode = argument.variadic_capture_mode(),
                      .has_default_materialisation = has_default_materialisation,
                      .materialisation_options = std::move(options),
                      .source_group = source_group});

    if (!argument.is_time_series()) {
        return true;
    }

    const auto *record = details::variadic_wiring_record(argument.time_series());
    if (record == nullptr) {
        if (type_cast<TimeSeriesBundleType>(&argument.time_series().type().target_type()) == nullptr) {
            return true;
        }

        const auto original_arguments =
            details::time_series_bundle_variadic_wiring_operations().original_arguments(argument.time_series());
        if (!original_arguments) {
            return true;
        }
        for (const auto &original : *original_arguments) {
            if (!append_argument_key(result, sources, original)) {
                return false;
            }
        }
        return true;
    }

    for (const auto &item : record->argument_descriptors()) {
        result.push_back({.type = &item.type(),
                          .present = item.present(),
                          .name = item.name(),
                          .requires_optional = item.requires_optional(),
                          .requires_valid_items = item.annotations().valid_items,
                          .variadic_capture_mode = item.variadic_mode()});
    }
    return true;
}

std::optional<std::vector<ArgumentKey>> argument_key(std::span<const WiringArgument> arguments) {
    auto result = std::vector<ArgumentKey>{};
    auto sources = std::vector<const details::WiringRecord *>{};
    result.reserve(arguments.size());

    for (const auto &argument : arguments) {
        if (!append_argument_key(result, sources, argument)) {
            return std::nullopt;
        }
    }

    return result;
}

std::optional<Scalar<>> scalar(std::span<const details::WiringArgumentDescriptor> arguments, std::size_t index) {
    if (index >= arguments.size()) {
        return std::nullopt;
    }

    const auto &argument = arguments[index];
    if (!argument.is_scalar() || !argument.present()) {
        return std::nullopt;
    }

    return argument.scalar();
}

using ScalarRead = details::OperatorScalarRead;

struct ScalarReads {
    const WiringTemplate *implementation{};
    bool cacheable{true};
    std::vector<ScalarRead> values;
};

struct OperatorResolutionAnalysis {
    details::OperatorResolutionResult resolution;
    ScalarReads scalar_reads;
};

void record_scalar_read(void *context, std::size_t index, Scalar<> value) {
    auto &reads = *static_cast<ScalarReads *>(context);
    if (!value.type().supports_equality() || !value.type().supports_hash()) {
        reads.cacheable = false;
        return;
    }

    if (std::ranges::any_of(reads.values, [&](const auto &read) {
            return read.implementation == reads.implementation && read.index == index;
        })) {
        return;
    }

    reads.values.push_back({reads.implementation, index, ScalarValue<>{value}});
}

} // namespace

Operator::Operator(Signature prototype, std::span<const Variable *const> type_parameters)
    : prototype_{std::move(prototype)}, type_parameters_{type_parameters.begin(), type_parameters.end()} {}

Operator::~Operator() = default;

void Operator::add_overload(std::unique_ptr<WiringTemplate> implementation) {
    if (!prototype_.has_output() && implementation->signature().has_output()) {
        throw std::invalid_argument{"an outputless operator cannot have an output overload"};
    }
    overloads_.push_back(std::move(implementation));
}

template <typename CandidateRange>
auto Operator::analyze_overloads(
    GraphWiring &wiring, std::span<const WiringArgument> arguments, const TypeMap *seed_type_map,
    const CandidateRange &candidates, bool record_reads) const {
    struct Candidate {
        const WiringTemplate *implementation;
        SignatureResolution resolution;
        std::vector<const Type *> argument_types;
        std::vector<std::optional<std::size_t>> supplied_argument_indices;
        std::vector<InputValidity> argument_validity;
        std::vector<unsigned char> argument_valid_items;
        std::vector<WiringArgumentFormMatch> argument_forms;
        double materialisation_cost;
        double binding_cost;
    };

    auto analysis = OperatorResolutionAnalysis{};
    auto &result = analysis.resolution;
    auto &scalar_reads = analysis.scalar_reads;
    const auto &prototype = signature();
    auto explicit_type_parameters = std::vector<const Variable *>{};
    if (seed_type_map != nullptr) {
        for (const auto *variable : type_parameters_) {
            if (seed_type_map->find(*variable) != nullptr) {
                explicit_type_parameters.push_back(variable);
            }
        }
    }

    const auto matches_prototype_parameter = [&](const Variable &variable, const ResolvedSignature &resolved) {
        const auto *mapped = std::get_if<const Type *>(seed_type_map->find(variable));
        const auto *variable_type = dynamic_cast<const Type *>(&variable);
        if (mapped == nullptr || *mapped == nullptr || variable_type == nullptr) {
            return false;
        }

        auto used = false;
        for (auto index = std::size_t{}; index < prototype.parameter_count(); ++index) {
            if (prototype.parameter(index).type != variable_type) {
                continue;
            }
            used = true;
            if (index >= resolved.parameters.size() || resolved.parameters[index].type != *mapped) {
                return false;
            }
        }
        return used;
    };

    const auto applicable_failure = [](const Candidate &candidate, std::string message) {
        return matched_failure(*candidate.implementation, candidate.resolution.signature, candidate.argument_forms,
                               candidate.materialisation_cost, WiringFailureStage::applicable, std::move(message));
    };

    auto applicable = std::vector<Candidate>{};
    auto failures = std::vector<WiringCandidateDiagnostic>{};
    applicable.reserve(std::ranges::size(candidates));
    failures.reserve(std::ranges::size(candidates));
    auto temporary_records = details::WiringRecordOverlay{wiring};

    for (auto variadic = false;; variadic = true) {
        for (const auto *implementation : candidates) {
            if (implementation->signature().has_variadic_parameters() != variadic) {
                continue;
            }

            const auto &signature = implementation->signature();
            auto preserved_inputs = std::vector<unsigned char>{};
            preserved_inputs.reserve(signature.parameter_count());
            for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
                preserved_inputs.push_back(static_cast<unsigned char>(implementation->preserves_input_point(index)));
            }
            auto normalized = details::NormalizedWiringArguments{};
            try {
                temporary_records.clear();
                normalized = details::probe_wiring_arguments(signature, arguments, temporary_records, &prototype);
            } catch (const std::exception &error) {
                failures.push_back(failure(*implementation, WiringFailureStage::argument_count, error.what()));
                continue;
            }

            auto argument_validity = std::vector<InputValidity>{};
            argument_validity.reserve(normalized.arguments.size());
            auto argument_valid_items = std::vector<unsigned char>{};
            argument_valid_items.reserve(normalized.arguments.size());
            for (const auto &argument : normalized.arguments) {
                argument_validity.push_back(argument.requires_optional() ? optional : required);
                argument_valid_items.push_back(static_cast<unsigned char>(argument.annotations().valid_items));
            }

            auto matches = std::vector<WiringSignatureMatch>{};
            try {
                matches = signature.match_wiring_arguments(normalized.arguments, seed_type_map);
            } catch (const TypeResolutionError &error) {
                failures.push_back(failure(*implementation, error));
                continue;
            }

            if (matches.empty()) {
                auto supplied_types = std::vector<const Type *>{};
                supplied_types.reserve(normalized.arguments.size());
                for (const auto &argument : normalized.arguments) {
                    if (!argument.is_time_series()) {
                        supplied_types.push_back(&argument.type());
                        continue;
                    }
                    const auto point = argument.time_series();
                    if (point.has_default_materialisation()) {
                        supplied_types.push_back(&argument.type());
                        continue;
                    }
                    const auto options = point.materialisation_options();
                    supplied_types.push_back(options.empty() ? &argument.type() : options.front().target_type);
                }
                try {
                    (void)signature.match_arguments(supplied_types, seed_type_map);
                    failures.push_back(failure(*implementation, WiringFailureStage::type_match,
                                               "no supplied form satisfies the signature"));
                } catch (const TypeResolutionError &error) {
                    failures.push_back(failure(*implementation, error));
                }
                continue;
            }

            for (auto &match : matches) {
                auto requirement = accept;
                auto requirement_exception = std::exception_ptr{};
                scalar_reads.implementation = implementation;
                const auto make_arguments = [&](const TypeMap &type_map) {
                    if (record_reads) {
                        return WiringArguments{signature, type_map, normalized.descriptors, &scalar_reads,
                                               &record_scalar_read};
                    }
                    return WiringArguments{signature, type_map, normalized.descriptors};
                };
                auto argument_context = make_arguments(match.type_map);
                auto completed = std::optional<ResolvedWiringSignatureMatch>{};
                auto observed_type_variables = std::vector<const Variable *>{};
                try {
                    completed.emplace(signature.complete_wiring_match(
                        std::move(match), argument_context, [&](const TypeMap &resolved_type_map) {
                            implementation->resolve_additional_types(resolved_type_map);
                            try {
                                requirement = implementation->requirement(make_arguments(resolved_type_map));
                            } catch (const details::ResolverTypeResolutionError &) {
                                throw;
                            } catch (...) {
                                requirement_exception = std::current_exception();
                            }
                        }, explicit_type_parameters.empty() ? nullptr : &observed_type_variables));
                } catch (const TypeResolutionError &error) {
                    failures.push_back(failure(*implementation, error));
                    continue;
                } catch (const std::exception &error) {
                    failures.push_back(failure(*implementation, WiringFailureStage::callback, error.what()));
                    throw WiringError{diagnostic(prototype, current_path(), arguments, std::move(failures)),
                                      std::current_exception()};
                } catch (...) {
                    failures.push_back(failure(*implementation, WiringFailureStage::callback,
                                               "non-standard exception"));
                    throw WiringError{diagnostic(prototype, current_path(), arguments, std::move(failures)),
                                      std::current_exception()};
                }

                if (requirement_exception != nullptr) {
                    try {
                        std::rethrow_exception(requirement_exception);
                    } catch (const std::exception &error) {
                        failures.push_back(matched_failure(*implementation, completed->resolution.signature,
                                                           completed->argument_forms,
                                                           completed->materialisation_cost,
                                                           WiringFailureStage::callback, error.what()));
                        throw WiringError{diagnostic(prototype, current_path(), arguments, std::move(failures)),
                                          requirement_exception};
                    } catch (...) {
                        failures.push_back(matched_failure(*implementation, completed->resolution.signature,
                                                           completed->argument_forms,
                                                           completed->materialisation_cost,
                                                           WiringFailureStage::callback,
                                                           "non-standard exception"));
                        throw WiringError{diagnostic(prototype, current_path(), arguments, std::move(failures)),
                                          requirement_exception};
                    }
                }

                if (!requirement.accepted()) {
                    failures.push_back(matched_failure(
                        *implementation, completed->resolution.signature, completed->argument_forms,
                        completed->materialisation_cost, WiringFailureStage::requirement,
                        requirement.message().empty() ? "requirement rejected the arguments"
                                                     : std::string{requirement.message()}));
                    continue;
                }

                const auto unused = std::ranges::find_if(explicit_type_parameters, [&](const auto *variable) {
                    return !std::ranges::contains(observed_type_variables, variable) &&
                           !matches_prototype_parameter(*variable, completed->resolution.signature);
                });
                if (unused != explicit_type_parameters.end()) {
                    failures.push_back(failure(*implementation, TypeResolutionError{
                        TypeResolutionErrorCode::mismatch, (*unused)->name(),
                        "overload does not accept the explicit type argument"}));
                    continue;
                }

                auto prepared_call = details::PreparedWiringCall{
                    std::move(*completed), std::span<const unsigned char>{preserved_inputs}};
                const auto binding_cost = prepared_call.binding_cost(normalized.arguments);
                auto candidate_types = std::vector<const Type *>{};
                candidate_types.reserve(normalized.arguments.size());
                for (auto index = std::size_t{}; index < normalized.arguments.size(); ++index) {
                    candidate_types.push_back(prepared_call.argument_type(index, normalized.arguments[index]));
                }
                applicable.push_back({implementation, std::move(prepared_call.resolution),
                                      std::move(candidate_types), normalized.supplied_argument_indices,
                                      argument_validity, argument_valid_items,
                                      std::move(prepared_call.argument_forms),
                                      prepared_call.materialisation_cost, binding_cost});
            }
        }

        if (!applicable.empty() || variadic) {
            break;
        }
    }

    const auto output_ambiguous = [&](const Candidate &candidate) {
        return std::ranges::any_of(applicable, [&](const auto &other) {
            return candidate.implementation == other.implementation &&
                   candidate.resolution.signature.output != other.resolution.signature.output;
        });
    };
    if (std::ranges::any_of(applicable, output_ambiguous)) {
        failures.clear();
        for (const auto &candidate : applicable) {
            if (output_ambiguous(candidate)) {
                failures.push_back(
                    applicable_failure(candidate, "viable forms resolve to different semantic output types"));
            }
        }
        result.ambiguous = true;
        result.failures = std::move(failures);
    } else {
        const auto same_concrete_binding = [&](const Candidate &left, const Candidate &right) {
            if (left.implementation != right.implementation ||
                left.resolution.signature.output != right.resolution.signature.output ||
                !left.implementation->same_binding_type_map(left.resolution.type_map, right.resolution.type_map)) {
                return false;
            }
            const auto &signature = left.implementation->signature();
            for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
                if (left.implementation->preserves_input_point(index)) {
                    continue;
                }
                if (left.resolution.signature.parameters[index].type !=
                        right.resolution.signature.parameters[index].type ||
                    left.argument_forms[index].materialisation_target !=
                        right.argument_forms[index].materialisation_target) {
                    return false;
                }
            }
            return true;
        };
        auto unique_applicable = std::vector<Candidate>{};
        unique_applicable.reserve(applicable.size());
        for (auto &candidate : applicable) {
            const auto existing = std::ranges::find_if(unique_applicable, [&](const auto &other) {
                return same_concrete_binding(candidate, other);
            });
            if (existing == unique_applicable.end()) {
                unique_applicable.push_back(std::move(candidate));
            } else if (candidate.binding_cost < existing->binding_cost) {
                *existing = std::move(candidate);
            }
        }
        applicable = std::move(unique_applicable);

        if (!applicable.empty()) {
            const auto lowest_cost =
                std::ranges::min_element(applicable, {}, &Candidate::binding_cost)->binding_cost;
            std::erase_if(applicable, [&](const auto &candidate) {
                return candidate.binding_cost != lowest_cost;
            });
        }

        Candidate *selected = nullptr;
        for (auto &candidate : applicable) {
            const auto least_generic = std::ranges::all_of(applicable, [&](const auto &other) {
                return &candidate == &other ||
                       preferred(*candidate.implementation, candidate.resolution.signature, *other.implementation,
                                 other.resolution.signature, candidate.argument_types, other.argument_types,
                                 candidate.supplied_argument_indices, other.supplied_argument_indices,
                                 candidate.argument_validity, other.argument_validity,
                                 candidate.argument_valid_items);
            });
            if (least_generic) {
                selected = &candidate;
                break;
            }
        }

        if (selected == nullptr) {
            result.ambiguous = !applicable.empty();
            if (result.ambiguous) {
                failures.clear();
                for (const auto &candidate : applicable) {
                    failures.push_back(applicable_failure(candidate, "applicable"));
                }
            }
            result.failures = std::move(failures);
        } else {
            auto selected_forms = std::vector<const TimeSeriesType *>{};
            selected_forms.reserve(selected->argument_forms.size());
            for (const auto &form : selected->argument_forms) {
                selected_forms.push_back(form.materialisation_target);
            }
            result.selected.emplace(details::OperatorResolutionMatch{
                selected->implementation,
                {std::move(selected->resolution.signature), std::move(selected->resolution.type_map)},
                std::move(selected_forms), std::move(selected->supplied_argument_indices)});
            result.failures = std::move(failures);
        }
    }

    return analysis;
}

details::OperatorResolutionResult Operator::resolve_overloads(
    GraphWiring &wiring, std::span<const WiringArgument> arguments, const TypeMap *seed_type_map,
    std::span<const WiringTemplate *const> candidates) const {
    auto analysis = analyze_overloads(wiring, arguments, seed_type_map, candidates, false);
    return std::move(analysis.resolution);
}

std::optional<WiringPoint> Operator::invoke(GraphWiring &wiring, std::span<const WiringArgument> arguments,
                                            const TypeMap *seed_type_map) const {
    auto &cache = wiring.operator_resolution_cache(*this);
    if (cache.operator_identity != cache_identity_ || cache.overload_count != overloads_.size()) {
        cache.entries.clear();
        cache.operator_identity = cache_identity_;
        cache.overload_count = overloads_.size();
    }

    const auto &prototype = signature();
    auto cache_key = argument_key(arguments);
    if (cache_key) {
        auto sources = std::vector<const details::WiringRecord *>{};
        for (const auto &argument : arguments) {
            if (argument.is_time_series()) {
                (void)source_group_index(sources, argument.time_series().record()->source_identity());
            }
        }
        auto context_names = std::vector<util::symbol>{};
        auto context_cacheable = true;
        for (const auto &implementation : overloads_) {
            if (!context_cacheable) {
                break;
            }
            const auto &signature = implementation->signature();
            for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
                if (!context_cacheable) {
                    break;
                }
                const auto &parameter = signature.parameter(index);
                if (!parameter.context || std::ranges::find(context_names, parameter.name) != context_names.end()) {
                    continue;
                }
                context_names.push_back(parameter.name);

                const auto explicit_argument = std::ranges::any_of(arguments, [&](const auto &argument) {
                    return argument.name() == std::optional{parameter.name} &&
                           argument.variadic_capture_mode() == details::VariadicMode::none;
                });
                const auto implicit =
                    explicit_argument ? std::optional<WiringArgument>{} : details::ContextScope::find(parameter.name);
                if (implicit) {
                    auto context_key = std::vector<ArgumentKey>{};
                    if (!append_argument_key(context_key, sources, *implicit)) {
                        context_cacheable = false;
                        break;
                    }
                    context_key.front().name = parameter.name;
                    cache_key->insert(cache_key->end(), std::make_move_iterator(context_key.begin()),
                                      std::make_move_iterator(context_key.end()));
                } else {
                    cache_key->push_back({.type = nullptr,
                                          .present = false,
                                          .name = parameter.name,
                                          .source_group = static_cast<std::size_t>(-1)});
                }
            }
        }
        if (!context_cacheable) {
            cache_key.reset();
        }
    }
    auto seed_key = std::optional<TypeMap>{};
    if (seed_type_map != nullptr) {
        seed_key.emplace(seed_type_map->copy());
    }

    if (cache_key) {
        auto temporary_records = details::WiringRecordOverlay{wiring};
        for (const auto &entry : cache.entries) {
            const auto seed_matches = entry.seed_type_map.has_value() == seed_key.has_value() &&
                                      (!seed_key || entry.seed_type_map->has_same_mappings(*seed_key));
            if (!seed_matches || !std::ranges::equal(entry.arguments, *cache_key)) {
                continue;
            }

            auto scalar_values_match = true;
            auto read_implementation = static_cast<const WiringTemplate *>(nullptr);
            auto probe = details::NormalizedWiringArguments{};
            for (const auto &read : entry.scalar_reads) {
                if (read.implementation != read_implementation) {
                    try {
                        probe = {};
                        temporary_records.clear();
                        probe = details::probe_wiring_arguments(read.implementation->signature(), arguments,
                                                                temporary_records, &prototype);
                    } catch (const std::exception &) {
                        scalar_values_match = false;
                        break;
                    }
                    read_implementation = read.implementation;
                }

                const auto current = scalar(probe.descriptors, read.index);
                if (!current || !read.value.view().equals(*current)) {
                    scalar_values_match = false;
                    break;
                }
            }
            if (!scalar_values_match) {
                continue;
            }

            if (entry.argument_forms.size() != entry.implementation->signature().parameter_count()) {
                continue;
            }
            auto committed = details::NormalizedWiringArguments{};
            try {
                committed = details::normalize_wiring_arguments(entry.implementation->signature(), arguments,
                                                                wiring.allocation(), &prototype);
            } catch (const std::exception &) {
                continue;
            }

            auto resolution = SignatureResolution{entry.resolution.signature, entry.resolution.type_map.copy()};
            const auto &signature = entry.implementation->signature();
            details::bind_materialisation_forms(wiring, *entry.implementation, committed.arguments,
                                                entry.argument_forms);
            require_concrete_materialisation_forms(wiring, *entry.implementation, committed.arguments,
                                                   entry.argument_forms);
            return invoke_resolved(wiring, *entry.implementation, std::move(resolution), committed.arguments,
                                   committed.descriptors, prototype.name());
        }
    }

    auto candidates = overloads_ | std::views::transform([](const auto &implementation) {
                         return implementation.get();
                     });
    auto analysis = analyze_overloads(wiring, arguments, seed_type_map, candidates, true);
    auto &resolution = analysis.resolution;
    if (!resolution.selected) {
        throw WiringError{diagnostic(prototype, current_path(), arguments, std::move(resolution.failures),
                                     resolution.ambiguous)};
    }
    auto &selected = *resolution.selected;

    if (analysis.scalar_reads.cacheable && cache_key) {
        if (cache.entries.size() == details::OperatorResolutionCache::maximum_entries) {
            cache.entries.pop_front();
        }
        cache.entries.push_back({std::move(*cache_key), std::move(seed_key),
                                 std::move(analysis.scalar_reads.values),
                                 selected.implementation,
                                 {selected.resolution.signature, selected.resolution.type_map.copy()},
                                 selected.argument_forms});
    }

    auto committed = details::normalize_wiring_arguments(selected.implementation->signature(), arguments,
                                                         wiring.allocation(), &prototype);
    details::bind_materialisation_forms(wiring, *selected.implementation, committed.arguments,
                                        selected.argument_forms);
    require_concrete_materialisation_forms(wiring, *selected.implementation, committed.arguments,
                                           selected.argument_forms);
    return invoke_resolved(wiring, *selected.implementation, std::move(selected.resolution), committed.arguments,
                           committed.descriptors, prototype.name());
}

std::optional<WiringPoint> Operator::wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                          std::span<const WiringArgument>) const {
    throw std::logic_error{"an operator must select an overload before wiring"};
}

} // namespace bg
