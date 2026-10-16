#include <bg/type/signature.hpp>

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_argument_descriptor.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/size_variable.hpp>
#include <bg/type/details/resolver_type_resolution_error.hpp>
#include <bg/type/schema.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bg {
namespace {

template <typename Category>
std::optional<bool> variable_accepts(const Variable &variable, const TypeMapValue &mapping) {
    const auto *typed_variable = dynamic_cast<const TypeVariableType<Category> *>(&variable);
    if (typed_variable == nullptr) {
        return std::nullopt;
    }

    const auto *type = std::get_if<const Type *>(&mapping);
    if (type == nullptr || *type == nullptr) {
        return false;
    }

    return typed_variable->accepts(**type);
}

bool valid_resolver_mapping(const Variable &variable, const TypeMapValue &mapping) {
    if (const auto result = variable_accepts<Type>(variable, mapping)) {
        return *result;
    }
    if (const auto result = variable_accepts<ScalarType>(variable, mapping)) {
        return *result;
    }
    if (const auto result = variable_accepts<TimeSeriesType>(variable, mapping)) {
        return *result;
    }
    if (const auto result = variable_accepts<SchemaType>(variable, mapping)) {
        return *result;
    }
    if (const auto *guarantee = dynamic_cast<const GuaranteeVariable *>(&variable)) {
        const auto *value = std::get_if<TimeSeriesGuarantee>(&mapping);
        return value != nullptr && (!guarantee->minimum() || satisfies(*value, *guarantee->minimum()));
    }
    if (dynamic_cast<const SizeVariable *>(&variable) != nullptr) {
        return std::holds_alternative<Size>(mapping);
    }

    return false;
}

const Type &source_type_without_guarantees(const Type &type);

const Schema &source_schema_without_guarantees(const Schema &schema_type) {
    if (!schema_type.is_resolved()) {
        return schema_type;
    }

    auto fields = std::vector<SchemaField>{};
    fields.reserve(schema_type.direct_fields().size());
    for (const auto &field : schema_type.direct_fields()) {
        fields.push_back({field.name, &source_type_without_guarantees(*field.type), field.optional});
    }

    const auto *base = schema_type.base() == nullptr ? nullptr : &source_schema_without_guarantees(*schema_type.base());
    return bg::schema(schema_type.name(), schema_type.identity(), base, fields);
}

const Type &source_type_without_guarantees(const Type &type) {
    if (const auto *schema_type = dynamic_cast<const Schema *>(&type)) {
        return source_schema_without_guarantees(*schema_type);
    }

    const auto *time_series = type_cast<TimeSeriesType>(&type);
    if (time_series == nullptr || !time_series->is_resolved()) {
        return type;
    }

    if (const auto *reference = type_cast<TimeSeriesReferenceType>(time_series)) {
        const auto &target = static_cast<const TimeSeriesType &>(
            source_type_without_guarantees(reference->target_type()));
        if (const auto *reference_target = type_cast<TimeSeriesReferenceType>(&target)) {
            return details::inherited_reference_transport_type(*reference_target);
        }
        return time_series_reference_type(target, TimeSeriesGuarantee::ordinary);
    }
    return time_series->with_guarantee_limit(TimeSeriesGuarantee::ordinary);
}

bool same_source_mapping(const TypeMapValue &left, const TypeMapValue &right) {
    const auto *left_type = std::get_if<const Type *>(&left);
    const auto *right_type = std::get_if<const Type *>(&right);
    if (left_type != nullptr && right_type != nullptr) {
        return *left_type != nullptr && *right_type != nullptr &&
               &source_type_without_guarantees(**left_type) == &source_type_without_guarantees(**right_type);
    }

    return left == right;
}

enum class ResolverState : unsigned char { pending, active, complete };

struct ResolverContext {
    std::span<const VariableResolver> resolvers;
    const WiringArguments *arguments;
    std::vector<ResolverState> states;
};

const TypeMapValue *resolve_missing_mapping(void *context, TypeMap &type_map, const Variable &variable) {
    auto &resolution = *static_cast<ResolverContext *>(context);
    try {
        const auto found = std::ranges::find_if(resolution.resolvers,
                                                [&](const auto &resolver) { return resolver.variable == &variable; });
        if (found == resolution.resolvers.end()) {
            return nullptr;
        }

        const auto index = static_cast<std::size_t>(found - resolution.resolvers.begin());
        if (resolution.states[index] == ResolverState::active) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, variable.name(),
                                      "variable resolver dependency cycle"};
        }
        if (resolution.states[index] == ResolverState::complete) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, variable.name(),
                                      "variable resolver completed without a mapping"};
        }
        if (found->requires_arguments && resolution.arguments == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, variable.name(),
                                      "variable resolver requires wiring arguments"};
        }

        resolution.states[index] = ResolverState::active;
        auto restore_state = util::scope_exit{[&]() noexcept { resolution.states[index] = ResolverState::pending; }};
        auto mapping = found->resolve(type_map, resolution.arguments);
        if (!valid_resolver_mapping(variable, mapping)) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, variable.name(),
                                      "variable resolver returned an incompatible value"};
        }

        type_map.set(variable, std::move(mapping));
        resolution.states[index] = ResolverState::complete;
        restore_state.release();

        return type_map.find(variable);
    } catch (const details::ResolverTypeResolutionError &) {
        throw;
    } catch (const TypeResolutionError &error) {
        throw details::ResolverTypeResolutionError{error};
    }
}

} // namespace

bool same_resolved_signature(const ResolvedSignature &left, const ResolvedSignature &right) noexcept {
    if (left.output != right.output || left.parameters.size() != right.parameters.size()) {
        return false;
    }

    return std::ranges::equal(left.parameters, right.parameters,
                              [](const auto &left_parameter, const auto &right_parameter) {
                                  return left_parameter.type == right_parameter.type;
                              });
}

void Signature::validate_variadic_parameters() const {
    auto positional_seen = false;
    auto named_seen = false;
    auto variadic_seen = false;

    for (auto index = std::size_t{}; index < parameters_.size(); ++index) {
        const auto mode = parameters_[index].variadic_mode;
        if (mode == details::VariadicMode::none) {
            if (variadic_seen) {
                throw std::invalid_argument{"a fixed parameter cannot follow a variadic parameter"};
            }
            continue;
        }

        variadic_seen = true;
        if (mode == details::VariadicMode::positional) {
            if (positional_seen) {
                throw std::invalid_argument{"a signature cannot have more than one var_args parameter"};
            }
            if (named_seen) {
                throw std::invalid_argument{"var_args cannot follow var_kwargs"};
            }
            positional_seen = true;
        } else {
            if (named_seen) {
                throw std::invalid_argument{"a signature cannot have more than one var_kwargs parameter"};
            }
            named_seen = true;
        }

        if (!parameters_[index].type->is_time_series() || parameters_[index].variadic_operations == nullptr) {
            throw std::invalid_argument{"parameter '" + std::string{parameters_[index].name.text()} +
                                        "' in signature '" + std::string{name_.text()} +
                                        "' does not support variadic wiring"};
        }
        parameters_[index].variadic_operations->validate_parameter(
            static_cast<const TimeSeriesType &>(*parameters_[index].type), mode);
    }
}

ResolvedSignature Signature::resolve(std::span<const Type *const> arguments) const {
    return std::move(resolve_with_type_map(arguments).signature);
}

SignatureResolution Signature::resolve_seeded(std::span<const Type *const> arguments, TypeMap type_map) const {
    type_map = match_arguments(arguments, &type_map);
    auto signature = complete_resolution(type_map);

    return {std::move(signature), std::move(type_map)};
}

TypeMap Signature::match_arguments(std::span<const Type *const> arguments, const TypeMap *seed) const {
    if (arguments.size() != parameters_.size()) {
        throw TypeResolutionError{TypeResolutionErrorCode::argument_count, name_,
                                  "signature received the wrong number of arguments"};
    }

    auto type_map = seed == nullptr ? TypeMap{} : seed->copy();

    const auto mismatch = [&](std::size_t index, std::string message) -> void {
        const auto &parameter = parameters_[index];
        throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name, std::move(message), parameter.type,
                                  arguments[index]};
    };

    auto unresolved = std::vector<std::size_t>{};
    unresolved.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto &parameter = parameters_[i];
        if (!arguments[i]->is_resolved()) {
            unresolved.push_back(i);
            continue;
        }
        if (!parameter.type->match(*arguments[i], type_map)) {
            mismatch(i, "argument does not satisfy its generic type");
        }
    }

    const auto resolve_if_constrained = [](const Type &type, const TypeMap &map) -> const Type * {
        try {
            return &type.resolve(map);
        } catch (const TypeResolutionError &error) {
            if (error.code() == TypeResolutionErrorCode::unmapped_variable) {
                return nullptr;
            }
            throw;
        }
    };

    while (!unresolved.empty()) {
        auto progress = false;
        for (auto position = unresolved.begin(); position != unresolved.end();) {
            const auto index = *position;
            const auto &parameter = *parameters_[index].type;
            const auto &argument = *arguments[index];
            auto trial = type_map.overlay();

            if (const auto *resolved_argument = resolve_if_constrained(argument, trial)) {
                if (!parameter.match(*resolved_argument, trial)) {
                    mismatch(index, "argument does not satisfy its generic type");
                }
            } else if (const auto *resolved_parameter = resolve_if_constrained(parameter, trial)) {
                if (!argument.match(*resolved_parameter, trial)) {
                    mismatch(index, "generic argument cannot satisfy the resolved parameter type");
                }
            } else {
                const auto *parameter_series = type_cast<TimeSeriesType>(&parameter);
                const auto *argument_series = type_cast<TimeSeriesType>(&argument);
                if (parameter_series == nullptr || argument_series == nullptr) {
                    ++position;
                    continue;
                }

                const auto *parameter_structure = static_cast<const TimeSeriesType *>(nullptr);
                try {
                    parameter_structure = &parameter_series->with_guarantee(TimeSeriesGuarantee::ordinary);
                } catch (const TypeResolutionError &error) {
                    if (error.code() != TypeResolutionErrorCode::invalid_type_map) {
                        throw;
                    }
                }

                if (parameter_structure != nullptr && parameter_structure->is_resolved()) {
                    if (!argument.match(*parameter_structure, trial)) {
                        mismatch(index, "generic argument cannot satisfy the parameter structure");
                    }
                } else if (const auto *argument_structure = resolve_if_constrained(argument, trial)) {
                    if (!parameter.match(*argument_structure, trial)) {
                        mismatch(index, "argument structure does not satisfy the generic parameter");
                    }
                } else {
                    ++position;
                    continue;
                }
            }

            if (const auto *resolved_argument = resolve_if_constrained(argument, trial)) {
                if (!parameter.match(*resolved_argument, trial)) {
                    mismatch(index, "generic argument does not satisfy the parameter type");
                }
            }

            const auto *resolved_parameter = resolve_if_constrained(parameter, trial);
            const auto *resolved_argument = resolve_if_constrained(argument, trial);
            if (resolved_parameter == nullptr || resolved_argument == nullptr ||
                !resolved_parameter->match(*resolved_argument, trial)) {
                mismatch(index, "generic argument does not resolve to the parameter type");
            }

            type_map.apply(std::move(trial));
            position = unresolved.erase(position);
            progress = true;
        }

        if (!progress) {
            mismatch(unresolved.front(), "argument and parameter leave their type variables unconstrained");
        }
    }

    return type_map;
}

void Signature::validate_graph_constraints(std::span<const WiringArgument> arguments) const {
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto &parameter = parameters_[index];
        if (!parameter.graph_constraint) {
            continue;
        }
        if (!arguments[index].is_graph() || !parameter.graph_constraint->matches(arguments[index].graph())) {
            throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name,
                                      "graph structure does not match the declared graph constraint",
                                      parameter.type, &arguments[index].type()};
        }
    }
}

SignatureResolution Signature::resolve_with_type_map(std::span<const Type *const> arguments) const {
    return resolve_seeded(arguments, TypeMap{});
}

SignatureResolution Signature::resolve_with_type_map(std::span<const Type *const> arguments,
                                                     const TypeMap &type_map) const {
    return resolve_seeded(arguments, type_map.copy());
}

SignatureResolution Signature::resolve_wiring_arguments(std::span<const WiringArgument> arguments,
                                                        std::span<const details::WiringArgumentDescriptor> descriptors,
                                                        const TypeMap *seed,
                                                        const std::function<void(const TypeMap &)> &completion) const {
    validate_wiring_arguments(arguments);

    auto argument_types = std::vector<const Type *>{};
    argument_types.reserve(arguments.size());
    for (const auto &argument : arguments) {
        argument_types.push_back(&argument.type());
    }

    auto type_map = match_arguments(argument_types, seed);
    validate_graph_constraints(arguments);
    auto wiring_arguments = WiringArguments{*this, type_map, descriptors};
    auto signature = complete_resolution(type_map, &wiring_arguments, completion);
    return {std::move(signature), std::move(type_map)};
}

std::vector<WiringSignatureMatch>
Signature::match_wiring_arguments(std::span<const WiringArgument> arguments, const TypeMap *seed) const {
    validate_wiring_arguments(arguments);
    if (arguments.size() != parameters_.size()) {
        throw TypeResolutionError{TypeResolutionErrorCode::argument_count, name_,
                                  "signature received the wrong number of arguments"};
    }
    auto forms = std::vector<std::vector<WiringArgumentFormMatch>>(arguments.size());
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        auto &argument_forms = forms[index];
        const auto has_point = argument.is_time_series();
        const auto point = has_point ? argument.time_series() : WiringPoint{};

        if (!has_point || point.has_default_materialisation()) {
            argument_forms.push_back({index, &argument.type(), nullptr, 0.0});
        }

        if (!has_point) {
            continue;
        }

        for (const auto &option : point.materialisation_options()) {
            argument_forms.push_back({index, option.target_type, option.target_type, option.cost});
        }

        if (argument_forms.empty()) {
            return {};
        }
    }

    auto source_constraints = TypeMap{};
    const auto has_alternate_form = [&] {
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            if (std::ranges::any_of(forms[index], [&](const auto &form) {
                    return form.offered_type != &arguments[index].type();
                })) {
                return true;
            }
        }
        return false;
    }();
    if (has_alternate_form) {
        auto source_matches = std::vector<TypeMap>{};
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            auto source_map = TypeMap{};
            source_map.matching_structure_only_ = true;
            const auto &source_type = source_type_without_guarantees(arguments[index].type());
            if (parameters_[index].type->match(source_type, source_map)) {
                source_matches.push_back(std::move(source_map));
            }
        }

        auto source_mappings = std::unordered_map<const Variable *, TypeMapValue>{};
        auto conflicting_variables = std::unordered_set<const Variable *>{};
        for (const auto &source_map : source_matches) {
            for (const auto &[variable, mapping] : source_map.values_) {
                if (dynamic_cast<const GuaranteeVariable *>(variable) != nullptr) {
                    continue;
                }

                const auto [found, inserted] = source_mappings.try_emplace(variable, mapping);
                if (!inserted && !same_source_mapping(found->second, mapping)) {
                    conflicting_variables.insert(variable);
                }
            }
        }

        for (const auto &[variable, mapping] : source_mappings) {
            if (!conflicting_variables.contains(variable)) {
                source_constraints.set(*variable, mapping);
            }
        }
    }

    auto matches = std::vector<WiringSignatureMatch>{};
    auto offered_types = std::vector<const Type *>(arguments.size());
    auto selected_forms = std::vector<WiringArgumentFormMatch>(arguments.size());
    const auto materialisation_cost = [&] {
        auto bindings = std::vector<std::pair<const details::WiringRecord *, const TimeSeriesType *>>{};
        auto total = 0.0;
        for (const auto &form : selected_forms) {
            if (form.materialisation_target == nullptr) {
                continue;
            }

            const auto point = arguments[form.argument_index].time_series();
            const auto binding = std::pair{point.record()->source_identity(), form.materialisation_target};
            if (std::ranges::find(bindings, binding) != bindings.end()) {
                continue;
            }

            bindings.push_back(binding);
            total += form.materialisation_cost;
        }
        return total;
    };
    const auto enumerate = [&](auto &&self, std::size_t index) -> void {
        if (index == arguments.size()) {
            try {
                auto candidate_mappings = match_arguments(offered_types, seed);
                const auto matches_source = std::ranges::all_of(
                    source_constraints.values_, [&](const auto &entry) {
                        const auto *mapping = candidate_mappings.find(*entry.first);
                        return mapping != nullptr && same_source_mapping(*mapping, entry.second);
                    });
                if (matches_source) {
                    matches.push_back(
                        {std::move(candidate_mappings), selected_forms, materialisation_cost()});
                }
            } catch (const TypeResolutionError &error) {
                if (error.code() != TypeResolutionErrorCode::mismatch) {
                    throw;
                }
            }
            return;
        }

        for (const auto &form : forms[index]) {
            offered_types[index] = form.offered_type;
            selected_forms[index] = form;
            self(self, index + 1);
        }
    };
    enumerate(enumerate, 0);

    if (!matches.empty()) {
        validate_graph_constraints(arguments);
    }

    return matches;
}

void Signature::validate_wiring_arguments(std::span<const WiringArgument> arguments) const {
    if (arguments.size() != parameters_.size()) {
        throw TypeResolutionError{TypeResolutionErrorCode::argument_count, name_,
                                  "signature received the wrong number of wiring arguments"};
    }

    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        const auto &parameter = parameters_[index];
        if (argument.requires_optional() && parameter.validity != optional) {
            throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name,
                                      "an opt-qualified argument requires an optional input parameter",
                                      parameter.type, &argument.type()};
        }
        if (argument.is_scalar() && !argument.present() && !parameter.accepts_absent_scalar) {
            throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name,
                                      "a required scalar parameter has no value", parameter.type, &argument.type()};
        }
    }
}

ResolvedWiringSignatureMatch Signature::complete_wiring_match(
    WiringSignatureMatch match, const WiringArguments &argument_context,
    const std::function<void(const TypeMap &)> &completion) const {
    auto arguments = argument_context.with_type_map(match.type_map);
    auto signature = complete_resolution(match.type_map, &arguments, completion);
    auto resolution = SignatureResolution{std::move(signature), std::move(match.type_map)};
    return {std::move(resolution), std::move(match.argument_forms), match.materialisation_cost};
}

ResolvedSignature Signature::complete_resolution(
    TypeMap &type_map, const WiringArguments *arguments,
    const std::function<void(const TypeMap &)> &completion) const {
    auto resolver_context = ResolverContext{resolvers_, arguments, std::vector<ResolverState>(resolvers_.size())};
    type_map.missing_mapping_handler(&resolver_context, &resolve_missing_mapping);
    auto clear_handler = util::scope_exit{[&]() noexcept { type_map.missing_mapping_handler(nullptr, nullptr); }};

    std::vector<ResolvedParameter> parameters;
    parameters.reserve(parameters_.size());

    for (const Parameter &parameter : parameters_) {
        const Type &type = parameter.type->resolve(type_map);

        if (!type.is_scalar() && !type.is_time_series() && !type.is_graph()) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, parameter.name,
                                      "parameter resolved outside its supported categories"};
        }

        parameters.push_back({parameter.name, &type, parameter.activity, parameter.validity,
                              parameter.accepts_absent_scalar, parameter.variadic_mode, parameter.context,
                              parameter.dynamic_activity});
    }

    const TimeSeriesType *output = nullptr;
    if (output_ != nullptr) {
        const Type &resolved_output = output_->resolve(type_map);
        if (!resolved_output.is_time_series()) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name_,
                                      "output did not resolve to a time series type"};
        }

        output = static_cast<const TimeSeriesType *>(&resolved_output);
    }

    auto signature = ResolvedSignature{name_, std::move(parameters), output};
    if (completion) {
        completion(type_map);
    }

    return signature;
}

} // namespace bg
