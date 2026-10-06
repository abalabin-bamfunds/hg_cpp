#include <bg/graph/operator.hpp>

#include <bg/graph/context.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <compare>
#include <deque>
#include <exception>
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

std::partial_ordering compare_reference_affinity(const ResolvedSignature &left, const ResolvedSignature &right,
                                                 std::span<const Type *const> left_arguments,
                                                 std::span<const Type *const> right_arguments,
                                                 std::size_t parameter_count) noexcept {
    auto left_better = false;
    auto right_better = false;
    for (auto index = std::size_t{}; index < parameter_count; ++index) {
        const auto *left_argument = type_cast<TimeSeriesType>(left_arguments[index]);
        const auto *right_argument = type_cast<TimeSeriesType>(right_arguments[index]);
        const auto *left_parameter = type_cast<TimeSeriesType>(left.parameters[index].type);
        const auto *right_parameter = type_cast<TimeSeriesType>(right.parameters[index].type);
        if (left_argument == nullptr || right_argument == nullptr || left_parameter == nullptr ||
            right_parameter == nullptr) {
            continue;
        }

        const auto *left_argument_reference = type_cast<TimeSeriesReferenceType>(left_argument);
        const auto *right_argument_reference = type_cast<TimeSeriesReferenceType>(right_argument);
        const auto *left_reference = type_cast<TimeSeriesReferenceType>(left_parameter);
        const auto *right_reference = type_cast<TimeSeriesReferenceType>(right_parameter);
        const auto left_matches = (left_reference != nullptr) == (left_argument_reference != nullptr);
        const auto right_matches = (right_reference != nullptr) == (right_argument_reference != nullptr);
        left_better |= left_matches && !right_matches;
        right_better |= right_matches && !left_matches;

        const auto &left_supplied =
            left_argument_reference == nullptr ? *left_argument : left_argument_reference->target_type();
        const auto &right_supplied =
            right_argument_reference == nullptr ? *right_argument : right_argument_reference->target_type();
        const auto &left_target = left_reference == nullptr ? *left_parameter : left_reference->target_type();
        const auto &right_target = right_reference == nullptr ? *right_parameter : right_reference->target_type();
        const auto left_requires_representation = left_target.requires_reference_representation_for(left_supplied);
        const auto right_requires_representation = right_target.requires_reference_representation_for(right_supplied);
        left_better |= !left_requires_representation && right_requires_representation;
        right_better |= !right_requires_representation && left_requires_representation;
    }
    if (left_better == right_better) {
        return left_better ? std::partial_ordering::unordered : std::partial_ordering::equivalent;
    }
    return left_better ? std::partial_ordering::less : std::partial_ordering::greater;
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
        const auto reference_affinity = compare_reference_affinity(candidate_signature, other_signature,
                                                                   candidate_arguments, other_arguments, shared_fixed);
        if (reference_affinity != std::partial_ordering::equivalent) {
            return reference_affinity == std::partial_ordering::less;
        }
        if (candidate_fixed == other_fixed) {
            const auto specificity = compare_specificity(candidate.signature(), other.signature());
            if (specificity != std::partial_ordering::equivalent) {
                return specificity == std::partial_ordering::less;
            }
            const auto shared_parameters =
                std::min(candidate_signature.parameters.size(), other_signature.parameters.size());
            return compare_input_validity_preference(candidate_signature, other_signature,
                                                     candidate_argument_validity, other_argument_validity,
                                                     shared_parameters) ==
                   std::partial_ordering::less;
        }

        const auto shared_specificity =
            details::compare_parameter_specificity(candidate.signature(), other.signature(), shared_fixed);
        const auto prefix_length =
            candidate_fixed > other_fixed ? std::partial_ordering::less : std::partial_ordering::greater;
        const auto preferences_agree =
            shared_specificity == std::partial_ordering::equivalent || shared_specificity == prefix_length;
        return preferences_agree && prefix_length == std::partial_ordering::less;
    }
    const auto shared_parameters = std::min(candidate_signature.parameters.size(), other_signature.parameters.size());
    const auto reference_affinity = compare_reference_affinity(candidate_signature, other_signature,
                                                               candidate_arguments, other_arguments, shared_parameters);
    if (reference_affinity != std::partial_ordering::equivalent) {
        return reference_affinity == std::partial_ordering::less;
    }
    const auto specificity = compare_specificity(candidate.signature(), other.signature());
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
        parameters.push_back({parameter.name, parameter.type, {}, parameter.validity});
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

WiringDiagnostic diagnostic(const Signature &prototype, std::vector<util::symbol> path,
                            std::span<const WiringArgument> supplied, std::vector<WiringCandidateDiagnostic> candidates,
                            bool ambiguous = false) {
    auto arguments = std::vector<WiringParameterDiagnostic>{};
    arguments.reserve(supplied.size());
    for (auto index = std::size_t{}; index < supplied.size(); ++index) {
        const auto name =
            supplied[index].name().value_or(index < prototype.parameter_count() ? prototype.parameter(index).name
                                                                                : util::symbol{std::to_string(index)});
        auto type_owner =
            supplied[index].is_graph()
                ? std::shared_ptr<const Type>{supplied[index].graph().exact_type_ptr()}
                : std::shared_ptr<const Type>{};
        const auto validity = supplied[index].requires_optional() ? InputValidity::optional : InputValidity::required;
        arguments.push_back({name, &supplied[index].type(), std::move(type_owner), validity});
    }

    return {prototype.name(), std::move(path), std::move(arguments), std::move(candidates), ambiguous};
}

struct ArgumentKey {
    const Type *type{};
    std::shared_ptr<const GraphArgumentType> graph_type;
    bool present;
    std::optional<util::symbol> name;
    bool requires_optional;
    details::VariadicMode variadic_capture_mode;

    bool operator==(const ArgumentKey &other) const {
        if (present != other.present || name != other.name || requires_optional != other.requires_optional ||
            variadic_capture_mode != other.variadic_capture_mode ||
            static_cast<bool>(graph_type) != static_cast<bool>(other.graph_type)) {
            return false;
        }
        if (!graph_type) {
            return type == other.type;
        }
        return graph_type->same_constraint(*other.graph_type);
    }
};

bool append_argument_key(std::vector<ArgumentKey> &result, const WiringArgument &argument) {
    if (argument.is_graph()) {
        const auto &graph = argument.graph();
        if (!graph.exact_type().cacheable_constraint()) {
            return false;
        }
        result.push_back({.type = nullptr,
                          .graph_type = graph.exact_type_ptr(),
                          .present = true,
                          .name = argument.name(),
                          .requires_optional = false,
                          .variadic_capture_mode = argument.variadic_capture_mode()});
        return true;
    }

    result.push_back({.type = &argument.type(),
                      .graph_type = {},
                      .present = argument.present(),
                      .name = argument.name(),
                      .requires_optional = argument.requires_optional(),
                      .variadic_capture_mode = argument.variadic_capture_mode()});
    if (!argument.is_time_series()) {
        return true;
    }

    const auto *record = details::variadic_wiring_record(argument.time_series());
    if (record == nullptr) {
        return true;
    }

    const auto descriptors = record->argument_descriptors();
    for (const auto &item : descriptors) {
        result.push_back({.type = &item.type(),
                          .graph_type = {},
                          .present = item.present(),
                          .name = item.name(),
                          .requires_optional = item.requires_optional(),
                          .variadic_capture_mode = item.variadic_mode()});
    }
    return true;
}

std::optional<std::vector<ArgumentKey>> argument_key(std::span<const WiringArgument> arguments) {
    auto result = std::vector<ArgumentKey>{};
    result.reserve(arguments.size());

    for (const auto &argument : arguments) {
        if (!append_argument_key(result, argument)) {
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
        auto context_names = std::vector<util::symbol>{};
        for (const auto &implementation : overloads_) {
            const auto &signature = implementation->signature();
            for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
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
                cache_key->push_back({.type = implicit ? &implicit->type() : nullptr,
                                      .graph_type = {},
                                      .present = implicit.has_value(),
                                      .name = parameter.name,
                                      .requires_optional = implicit && implicit->requires_optional(),
                                      .variadic_capture_mode = details::VariadicMode::none});
            }
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

            auto committed = details::NormalizedWiringArguments{};
            try {
                committed = details::normalize_wiring_arguments(entry.implementation->signature(), arguments,
                                                                wiring.allocation(), &prototype_);
            } catch (const std::exception &) {
                continue;
            }

            auto resolution = SignatureResolution{entry.resolution.signature, entry.resolution.type_map.copy()};
            return invoke_resolved(wiring, *entry.implementation, std::move(resolution), committed.arguments,
                                   prototype_.name());
        }
    }

    struct Candidate {
        const WiringTemplate *implementation;
        SignatureResolution resolution;
        std::vector<const Type *> argument_types;
        std::vector<InputValidity> argument_validity;
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

            auto resolution = std::optional<SignatureResolution>{};
            auto normalized = details::NormalizedWiringArguments{};
            auto candidate_types = std::vector<const Type *>{};
            auto candidate_validity = std::vector<InputValidity>{};
            try {
                temporary_records.clear();
                normalized = details::probe_wiring_arguments(implementation->signature(), arguments,
                                                             temporary_records, &prototype_);
            } catch (const std::exception &error) {
                failures.push_back(failure(*implementation, WiringFailureStage::argument_count, error.what()));
                continue;
            }
            try {
                implementation->signature().validate_wiring_arguments(normalized.arguments);
                candidate_types.reserve(normalized.arguments.size());
                candidate_validity.reserve(normalized.arguments.size());
                for (const auto &argument : normalized.arguments) {
                    candidate_types.push_back(&argument.type());
                    candidate_validity.push_back(argument.requires_optional() ? optional : required);
                }
                scalar_reads.implementation = implementation.get();
                auto type_map = implementation->signature().match_arguments(candidate_types, seed_type_map);
                auto resolver_arguments = WiringArguments{implementation->signature(), type_map, normalized.descriptors,
                                                          &scalar_reads, &record_scalar_read};
                auto signature = implementation->signature().complete_resolution(type_map, &resolver_arguments);
                resolution.emplace(SignatureResolution{std::move(signature), std::move(type_map)});
            } catch (const TypeResolutionError &error) {
                failures.push_back(failure(*implementation, error));
                continue;
            } catch (const std::exception &error) {
                failures.push_back(failure(*implementation, WiringFailureStage::callback, error.what()));
                throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures))};
            }

            RequirementResult requirement = accept;
            try {
                requirement = implementation->requirement(WiringArguments{implementation->signature(),
                                                                          resolution->type_map, normalized.descriptors,
                                                                          &scalar_reads, &record_scalar_read});
            } catch (const std::exception &error) {
                failures.push_back(failure(*implementation, WiringFailureStage::callback, error.what()));
                throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures))};
            }

            if (!requirement.accepted()) {
                failures.push_back(failure(*implementation, WiringFailureStage::requirement,
                                           requirement.message().empty() ? "requirement rejected the arguments"
                                                                         : std::string{requirement.message()}));
                continue;
            }
            applicable.push_back(
                {implementation.get(), std::move(*resolution), std::move(candidate_types),
                 std::move(candidate_validity)});
        }

        if (!applicable.empty() || variadic) {
            break;
        }
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
                failures.push_back(failure(*candidate.implementation, WiringFailureStage::applicable, "applicable"));
            }
        }
        throw WiringError{diagnostic(prototype_, current_path(), arguments, std::move(failures), ambiguous)};
    }

    if (scalar_reads.cacheable && cache_key) {
        if (cache_->entries.size() == Cache::maximum_entries) {
            cache_->entries.pop_front();
        }
        cache_->entries.push_back({std::move(*cache_key),
                                   std::move(seed_key),
                                   std::move(scalar_reads.values),
                                   selected->implementation,
                                   {selected->resolution.signature, selected->resolution.type_map.copy()}});
    }

    auto committed = details::normalize_wiring_arguments(selected->implementation->signature(), arguments,
                                                         wiring.allocation(), &prototype_);
    return invoke_resolved(wiring, *selected->implementation, std::move(selected->resolution), committed.arguments,
                           prototype_.name());
}

std::optional<WiringPoint> Operator::wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                          std::span<const WiringArgument>) const {
    throw std::logic_error{"an operator must select an overload before wiring"};
}

} // namespace bg
