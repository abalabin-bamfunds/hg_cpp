#include <bg/graph/operator.hpp>

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

std::partial_ordering compare_reference_affinity(const Signature &left, const Signature &right,
                                                 std::span<const Type *const> arguments) noexcept {
    auto left_better = false;
    auto right_better = false;
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto argument_is_reference = dynamic_cast<const TimeSeriesReferenceType *>(arguments[index]) != nullptr;
        const auto left_matches = (dynamic_cast<const TimeSeriesReferenceType *>(left.parameter(index).type) !=
                                   nullptr) == argument_is_reference;
        const auto right_matches = (dynamic_cast<const TimeSeriesReferenceType *>(right.parameter(index).type) !=
                                    nullptr) == argument_is_reference;
        left_better |= left_matches && !right_matches;
        right_better |= right_matches && !left_matches;
    }
    if (left_better == right_better) {
        return left_better ? std::partial_ordering::unordered : std::partial_ordering::equivalent;
    }
    return left_better ? std::partial_ordering::less : std::partial_ordering::greater;
}

bool preferred(const WiringTemplate &candidate, const WiringTemplate &other, std::span<const Type *const> arguments) {
    const auto reference_affinity = compare_reference_affinity(candidate.signature(), other.signature(), arguments);
    if (reference_affinity != std::partial_ordering::equivalent) {
        return reference_affinity == std::partial_ordering::less;
    }
    const auto specificity = compare_specificity(candidate.signature(), other.signature());
    return specificity == std::partial_ordering::less;
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
        parameters.push_back({parameter.name, parameter.type});
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
                            std::span<const Type *const> argument_types,
                            std::vector<WiringCandidateDiagnostic> candidates, bool ambiguous = false) {
    auto arguments = std::vector<WiringParameterDiagnostic>{};
    arguments.reserve(argument_types.size());
    for (auto index = std::size_t{}; index < argument_types.size(); ++index) {
        const auto name =
            index < prototype.parameter_count() ? prototype.parameter(index).name : util::symbol{std::to_string(index)};
        arguments.push_back({name, argument_types[index]});
    }

    return {prototype.name(), std::move(path), std::move(arguments), std::move(candidates), ambiguous};
}

struct ArgumentKey {
    const Type *type;
    bool present;

    bool operator==(const ArgumentKey &) const noexcept = default;
};

std::vector<ArgumentKey> argument_key(std::span<const details::wiring_argument> arguments) {
    auto result = std::vector<ArgumentKey>{};
    result.reserve(arguments.size());

    for (const auto &argument : arguments) {
        result.push_back({&argument.type(), argument.present()});
    }

    return result;
}

bool matches(std::span<const ArgumentKey> key, std::span<const details::wiring_argument> arguments) {
    return key.size() == arguments.size() && std::ranges::equal(key, arguments, {}, {}, [](const auto &argument) {
               return ArgumentKey{&argument.type(), argument.present()};
           });
}

std::optional<Scalar<>> scalar(std::span<const details::wiring_argument> arguments, std::size_t index) {
    const auto &argument = arguments[index];
    if (!argument.is_scalar() || !argument.present()) {
        return std::nullopt;
    }

    if (const auto *view = argument.scalar_view()) {
        return Scalar<>::from(*view);
    }

    return Scalar<>::from(argument.scalar_runtime_type(), argument.scalar_data());
}

struct ScalarRead {
    std::size_t index;
    ScalarValue<> value;
};

struct ScalarReads {
    bool cacheable{true};
    std::vector<ScalarRead> values;
};

void record_scalar_read(void *context, std::size_t index, Scalar<> value) {
    auto &reads = *static_cast<ScalarReads *>(context);
    if (!value.type().supports_equality() || !value.type().supports_hash()) {
        reads.cacheable = false;
        return;
    }

    if (std::ranges::any_of(reads.values, [index](const auto &read) { return read.index == index; })) {
        return;
    }

    reads.values.push_back({index, ScalarValue<>{value}});
}

} // namespace

struct Operator::Cache {
    static constexpr auto maximum_entries = std::size_t{64};

    struct Entry {
        std::vector<ArgumentKey> arguments;
        std::vector<ScalarRead> scalar_reads;
        const WiringTemplate *implementation;
        SignatureResolution resolution;
    };

    std::deque<Entry> entries;
};

Operator::Operator(Signature prototype) : prototype_{std::move(prototype)}, cache_{std::make_unique<Cache>()} {}

Operator::~Operator() = default;

void Operator::add_overload(std::unique_ptr<WiringTemplate> implementation) {
    if (implementation->signature().has_output() != prototype_.has_output()) {
        throw std::invalid_argument{"an operator overload has the wrong output shape"};
    }
    overloads_.push_back(std::move(implementation));
    cache_->entries.clear();
}

std::optional<WiringPoint> Operator::invoke(GraphWiring &wiring, std::span<details::wiring_argument> arguments,
                                            const TypeMap *seed_type_map) const {
    if (seed_type_map == nullptr) {
        for (const auto &entry : cache_->entries) {
            if (!matches(entry.arguments, arguments)) {
                continue;
            }

            const auto scalar_values_match = std::ranges::all_of(entry.scalar_reads, [&](const auto &read) {
                const auto current = scalar(arguments, read.index);
                return current && read.value.view().equals(*current);
            });
            if (!scalar_values_match) {
                continue;
            }

            auto resolution = SignatureResolution{entry.resolution.signature, entry.resolution.type_map.copy()};
            return invoke_resolved(wiring, *entry.implementation, std::move(resolution), arguments, prototype_.name());
        }
    }

    auto argument_types = std::vector<const Type *>{};
    argument_types.reserve(arguments.size());
    for (const auto &argument : arguments) {
        argument_types.push_back(&argument.type());
    }

    struct Candidate {
        const WiringTemplate *implementation;
        SignatureResolution resolution;
    };

    auto applicable = std::vector<Candidate>{};
    auto failures = std::vector<WiringCandidateDiagnostic>{};
    applicable.reserve(overloads_.size());
    failures.reserve(overloads_.size());
    auto scalar_reads = ScalarReads{};

    for (const auto &implementation : overloads_) {
        auto resolution = std::optional<SignatureResolution>{};
        try {
            auto type_map = implementation->signature().match_arguments(argument_types, seed_type_map);
            auto resolver_arguments =
                WiringArguments{implementation->signature(), type_map, arguments, &scalar_reads, &record_scalar_read};
            auto signature = implementation->signature().complete_resolution(type_map, &resolver_arguments);
            resolution.emplace(SignatureResolution{std::move(signature), std::move(type_map)});
        } catch (const TypeResolutionError &error) {
            failures.push_back(failure(*implementation, error));
            continue;
        } catch (const std::exception &error) {
            failures.push_back(failure(*implementation, WiringFailureStage::callback, error.what()));
            throw WiringError{diagnostic(prototype_, current_path(), argument_types, std::move(failures))};
        }

        RequirementResult requirement = accept;
        try {
            requirement = implementation->requirement(WiringArguments{implementation->signature(), resolution->type_map,
                                                                      arguments, &scalar_reads, &record_scalar_read});
        } catch (const std::exception &error) {
            failures.push_back(failure(*implementation, WiringFailureStage::callback, error.what()));
            throw WiringError{diagnostic(prototype_, current_path(), argument_types, std::move(failures))};
        }

        if (!requirement.accepted()) {
            failures.push_back(failure(*implementation, WiringFailureStage::requirement,
                                       requirement.message().empty() ? "requirement rejected the arguments"
                                                                     : std::string{requirement.message()}));
            continue;
        }
        applicable.push_back({implementation.get(), std::move(*resolution)});
    }

    Candidate *selected = nullptr;
    for (auto &candidate : applicable) {
        const auto least_generic = std::ranges::all_of(applicable, [&](const auto &other) {
            return &candidate == &other || preferred(*candidate.implementation, *other.implementation, argument_types);
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
        throw WiringError{diagnostic(prototype_, current_path(), argument_types, std::move(failures), ambiguous)};
    }

    if (seed_type_map == nullptr && scalar_reads.cacheable) {
        if (cache_->entries.size() == Cache::maximum_entries) {
            cache_->entries.pop_front();
        }
        cache_->entries.push_back({argument_key(arguments),
                                   std::move(scalar_reads.values),
                                   selected->implementation,
                                   {selected->resolution.signature, selected->resolution.type_map.copy()}});
    }

    return invoke_resolved(wiring, *selected->implementation, std::move(selected->resolution), arguments,
                           prototype_.name());
}

std::optional<WiringPoint> Operator::wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                          std::span<details::wiring_argument>) const {
    throw std::logic_error{"an operator must select an overload before wiring"};
}

} // namespace bg
