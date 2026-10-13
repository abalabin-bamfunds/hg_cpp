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
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/details/resolver_type_resolution_error.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_variable.hpp>

#include <algorithm>
#include <compare>
#include <deque>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
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

std::partial_ordering compare_input_validity_preference(const ResolvedSignature &left, const ResolvedSignature &right,
                                                        std::span<const InputValidity> left_arguments,
                                                        std::span<const InputValidity> right_arguments,
                                                        std::size_t parameter_count) noexcept {
    auto left_better = false;
    auto right_better = false;
    for (auto index = std::size_t{}; index < parameter_count; ++index) {
        if (type_cast<TimeSeriesType>(left.parameters[index].type) == nullptr ||
            type_cast<TimeSeriesType>(right.parameters[index].type) == nullptr) {
            continue;
        }
        if (left_arguments[index] == optional || right_arguments[index] == optional) {
            continue;
        }

        const auto left_required = left.parameters[index].validity != optional;
        const auto right_required = right.parameters[index].validity != optional;
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
               std::span<const InputValidity> candidate_argument_validity,
               std::span<const InputValidity> other_argument_validity) {
    const auto candidate_variadic = candidate.signature().has_variadic_parameters();
    const auto other_variadic = other.signature().has_variadic_parameters();
    if (candidate_variadic != other_variadic) {
        return !candidate_variadic;
    }
    if (candidate_variadic) {
        const auto candidate_fixed = *candidate.signature().first_variadic_parameter();
        const auto other_fixed = *other.signature().first_variadic_parameter();
        const auto shared_fixed = std::min(candidate_fixed, other_fixed);
        const auto reference_affinity = details::compare_wiring_reference_affinity(
            candidate_signature, other_signature, candidate_arguments, other_arguments, shared_fixed);
        if (reference_affinity != std::partial_ordering::equivalent) {
            return reference_affinity == std::partial_ordering::less;
        }
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
            return compare_input_validity_preference(candidate_signature, other_signature,
                                                     candidate_argument_validity, other_argument_validity,
                                                     shared_parameters) == std::partial_ordering::less;
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
    const auto reference_affinity = details::compare_wiring_reference_affinity(
        candidate_signature, other_signature, candidate_arguments, other_arguments, shared_parameters);
    if (reference_affinity != std::partial_ordering::equivalent) {
        return reference_affinity == std::partial_ordering::less;
    }
    const auto parameter_count = candidate.signature().parameter_count();
    const auto specificity = parameter_count == other.signature().parameter_count()
                                 ? details::compare_parameter_specificity(candidate.signature(), other.signature(),
                                                                         candidate_arguments, other_arguments,
                                                                         parameter_count)
                                 : std::partial_ordering::unordered;
    if (specificity != std::partial_ordering::equivalent) {
        return specificity == std::partial_ordering::less;
    }
    return compare_input_validity_preference(candidate_signature, other_signature, candidate_argument_validity,
                                             other_argument_validity, shared_parameters) ==
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
        parameters.push_back({parameter.name, parameter.type, parameter.validity});
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
            details::preserves_union_wiring_input(implementation, index) ||
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
        auto argument = WiringParameterDiagnostic{name, &supplied[index].type(), validity};
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

struct ArgumentKey {
    const Type *type{};
    std::optional<GraphArgumentStructures> graph_structures;
    bool present;
    std::optional<util::symbol> name;
    bool requires_optional{};
    details::VariadicMode variadic_capture_mode{details::VariadicMode::none};
    bool has_default_materialisation{};
    std::vector<std::pair<const TimeSeriesType *, double>> materialisation_options;
    std::size_t source_group{static_cast<std::size_t>(-1)};

    bool operator==(const ArgumentKey &other) const {
        if (type != other.type || present != other.present || name != other.name ||
            requires_optional != other.requires_optional || variadic_capture_mode != other.variadic_capture_mode ||
            static_cast<bool>(graph_structures) != static_cast<bool>(other.graph_structures) ||
            has_default_materialisation != other.has_default_materialisation ||
            materialisation_options != other.materialisation_options || source_group != other.source_group) {
            return false;
        }
        if (graph_structures) {
            return *graph_structures == *other.graph_structures;
        }
        return true;
    }
};

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
                      .variadic_capture_mode = argument.variadic_capture_mode(),
                      .has_default_materialisation = has_default_materialisation,
                      .materialisation_options = std::move(options),
                      .source_group = source_group});

    if (!argument.is_time_series()) {
        return true;
    }

    const auto *record = details::variadic_wiring_record(argument.time_series());
    if (record == nullptr) {
        return true;
    }

    for (const auto &item : record->argument_descriptors()) {
        result.push_back({.type = &item.type(),
                          .present = item.present(),
                          .name = item.name(),
                          .requires_optional = item.requires_optional(),
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

struct ScalarRead {
    const WiringTemplate *implementation;
    std::size_t index;
    ScalarValue<> value;
};

struct ScalarReads {
    const WiringTemplate *implementation{};
    bool cacheable{true};
    std::vector<ScalarRead> values;
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

struct Operator::Cache {
    static constexpr auto maximum_entries = std::size_t{64};

    struct Entry {
        std::vector<ArgumentKey> arguments;
        std::optional<TypeMap> seed_type_map;
        std::vector<ScalarRead> scalar_reads;
        const WiringTemplate *implementation;
        SignatureResolution resolution;
        std::vector<const TimeSeriesType *> argument_forms;
    };

    std::deque<Entry> entries;
};

Operator::Operator(Signature prototype) : prototype_{std::move(prototype)}, cache_{std::make_unique<Cache>()} {}

Operator::~Operator() = default;

void Operator::add_overload(std::unique_ptr<WiringTemplate> implementation) {
    if (!prototype_.has_output() && implementation->signature().has_output()) {
        throw std::invalid_argument{"an outputless operator cannot have an output overload"};
    }
    overloads_.push_back(std::move(implementation));
    cache_->entries.clear();
}

std::optional<WiringPoint> Operator::invoke(GraphWiring &wiring, std::span<const WiringArgument> arguments,
                                            const TypeMap *seed_type_map) const {
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
        for (const auto &entry : cache_->entries) {
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
                                                                temporary_records, &prototype_);
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
                                                                wiring.allocation(), &prototype_);
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
                                   prototype_.name());
        }
    }

    struct Candidate {
        const WiringTemplate *implementation;
        SignatureResolution resolution;
        std::vector<const Type *> argument_types;
        std::vector<InputValidity> argument_validity;
        std::vector<WiringArgumentFormMatch> argument_forms;
        double materialisation_cost;
    };

    const auto applicable_failure = [](const Candidate &candidate, std::string message) {
        return matched_failure(*candidate.implementation, candidate.resolution.signature, candidate.argument_forms,
                               candidate.materialisation_cost, WiringFailureStage::applicable, std::move(message));
    };

    auto applicable = std::vector<Candidate>{};
    auto failures = std::vector<WiringCandidateDiagnostic>{};
    auto scalar_reads = ScalarReads{};
    applicable.reserve(overloads_.size());
    failures.reserve(overloads_.size());
    auto temporary_records = details::WiringRecordOverlay{wiring};

    for (auto variadic = false;; variadic = true) {
        for (const auto &implementation : overloads_) {
            if (implementation->signature().has_variadic_parameters() != variadic) {
                continue;
            }

            const auto &signature = implementation->signature();
            auto normalized = details::NormalizedWiringArguments{};
            try {
                temporary_records.clear();
                normalized = details::probe_wiring_arguments(signature, arguments, temporary_records, &prototype_);
            } catch (const std::exception &error) {
                failures.push_back(failure(*implementation, WiringFailureStage::argument_count, error.what()));
                continue;
            }

            auto argument_validity = std::vector<InputValidity>{};
            argument_validity.reserve(normalized.arguments.size());
            for (const auto &argument : normalized.arguments) {
                argument_validity.push_back(argument.requires_optional() ? optional : required);
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
                scalar_reads.implementation = implementation.get();
                auto argument_context = WiringArguments{signature, match.type_map, normalized.descriptors,
                                                        &scalar_reads, &record_scalar_read};
                auto completed = std::optional<ResolvedWiringSignatureMatch>{};
                try {
                    completed.emplace(signature.complete_wiring_match(
                        std::move(match), argument_context, [&](const TypeMap &resolved_type_map) {
                            implementation->resolve_additional_types(resolved_type_map);
                            try {
                                requirement = implementation->requirement(
                                    WiringArguments{signature, resolved_type_map, normalized.descriptors,
                                                    &scalar_reads, &record_scalar_read});
                            } catch (const details::ResolverTypeResolutionError &) {
                                throw;
                            } catch (...) {
                                requirement_exception = std::current_exception();
                            }
                        }));
                } catch (const TypeResolutionError &error) {
                    failures.push_back(failure(*implementation, error));
                    continue;
                } catch (const std::exception &error) {
                    failures.push_back(failure(*implementation, WiringFailureStage::callback, error.what()));
                    throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures)),
                                      std::current_exception()};
                } catch (...) {
                    failures.push_back(failure(*implementation, WiringFailureStage::callback,
                                               "non-standard exception"));
                    throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures)),
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
                        throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures)),
                                          requirement_exception};
                    } catch (...) {
                        failures.push_back(matched_failure(*implementation, completed->resolution.signature,
                                                           completed->argument_forms,
                                                           completed->materialisation_cost,
                                                           WiringFailureStage::callback,
                                                           "non-standard exception"));
                        throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures)),
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

                auto candidate_types = std::vector<const Type *>{};
                candidate_types.reserve(normalized.arguments.size());
                for (auto index = std::size_t{}; index < normalized.arguments.size(); ++index) {
                    candidate_types.push_back(details::preserves_union_wiring_input(*implementation, index)
                                                  ? &normalized.arguments[index].type()
                                                  : completed->argument_forms[index].offered_type);
                }
                applicable.push_back({implementation.get(), std::move(completed->resolution),
                                      std::move(candidate_types), argument_validity,
                                      std::move(completed->argument_forms),
                                      completed->materialisation_cost});
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
        throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures), true)};
    }

    const auto same_concrete_binding = [&](const Candidate &left, const Candidate &right) {
        if (left.implementation != right.implementation ||
            left.resolution.signature.output != right.resolution.signature.output) {
            return false;
        }
        const auto &signature = left.implementation->signature();
        for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
            if (details::preserves_union_wiring_input(*left.implementation, index)) {
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
        } else if (candidate.materialisation_cost < existing->materialisation_cost) {
            *existing = std::move(candidate);
        }
    }
    applicable = std::move(unique_applicable);

    if (!applicable.empty()) {
        const auto lowest_cost =
            std::ranges::min_element(applicable, {}, &Candidate::materialisation_cost)->materialisation_cost;
        std::erase_if(applicable, [&](const auto &candidate) {
            return candidate.materialisation_cost != lowest_cost;
        });
    }

    Candidate *selected = nullptr;
    for (auto &candidate : applicable) {
        const auto least_generic = std::ranges::all_of(applicable, [&](const auto &other) {
            return &candidate == &other ||
                   preferred(*candidate.implementation, candidate.resolution.signature, *other.implementation,
                             other.resolution.signature, candidate.argument_types, other.argument_types,
                             candidate.argument_validity, other.argument_validity);
        });
        if (least_generic) {
            selected = &candidate;
            break;
        }
    }

    if (selected == nullptr) {
        const auto ambiguous = !applicable.empty();
        if (ambiguous) {
            failures.clear();
            for (const auto &candidate : applicable) {
                failures.push_back(applicable_failure(candidate, "applicable"));
            }
        }
        throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures), ambiguous)};
    }

    if (scalar_reads.cacheable && cache_key) {
        if (cache_->entries.size() == Cache::maximum_entries) {
            cache_->entries.pop_front();
        }
        auto selected_forms = std::vector<const TimeSeriesType *>{};
        selected_forms.reserve(selected->argument_forms.size());
        for (const auto &form : selected->argument_forms) {
            selected_forms.push_back(form.materialisation_target);
        }
        cache_->entries.push_back({std::move(*cache_key), std::move(seed_key), std::move(scalar_reads.values),
                                   selected->implementation,
                                   {selected->resolution.signature, selected->resolution.type_map.copy()},
                                   std::move(selected_forms)});
    }

    auto committed = details::normalize_wiring_arguments(selected->implementation->signature(), arguments,
                                                         wiring.allocation(), &prototype_);
    const auto &signature = selected->implementation->signature();
    auto selected_forms = std::vector<const TimeSeriesType *>{};
    selected_forms.reserve(selected->argument_forms.size());
    for (const auto &form : selected->argument_forms) {
        selected_forms.push_back(form.materialisation_target);
    }
    details::bind_materialisation_forms(wiring, *selected->implementation, committed.arguments, selected_forms);
    require_concrete_materialisation_forms(wiring, *selected->implementation, committed.arguments, selected_forms);
    return invoke_resolved(wiring, *selected->implementation, std::move(selected->resolution), committed.arguments,
                           prototype_.name());
}

std::optional<WiringPoint> Operator::wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                          std::span<const WiringArgument>) const {
    throw std::logic_error{"an operator must select an overload before wiring"};
}

} // namespace bg
