#include <bg/type/signature.hpp>

#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/details/wiring_argument_descriptor.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/size_variable.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <stdexcept>

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
    if (const auto result = variable_accepts<BundleSchemaType>(variable, mapping)) {
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

enum class ResolverState : unsigned char { pending, active, complete };

struct ResolverContext {
    std::span<const VariableResolver> resolvers;
    const WiringArguments *arguments;
    std::vector<ResolverState> states;
};

const TypeMapValue *resolve_missing_mapping(void *context, TypeMap &type_map, const Variable &variable) {
    auto &resolution = *static_cast<ResolverContext *>(context);
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
}

} // namespace

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
        throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name, std::move(message),
                                  parameter.type, arguments[index]};
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
                    parameter_structure =
                        &parameter_series->with_guarantee(TimeSeriesGuarantee::ordinary);
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

SignatureResolution Signature::resolve_with_type_map(std::span<const Type *const> arguments) const {
    return resolve_seeded(arguments, TypeMap{});
}

SignatureResolution Signature::resolve_with_type_map(std::span<const Type *const> arguments,
                                                     const TypeMap &type_map) const {
    return resolve_seeded(arguments, type_map.copy());
}

SignatureResolution Signature::resolve_wiring_arguments(
    std::span<const WiringArgument> arguments,
    std::span<const details::WiringArgumentDescriptor> descriptors,
    const TypeMap *seed) const {
    auto argument_types = std::vector<const Type *>{};
    argument_types.reserve(arguments.size());
    for (const auto &argument : arguments) {
        argument_types.push_back(&argument.type());
    }

    auto type_map = match_arguments(argument_types, seed);
    auto wiring_arguments = WiringArguments{*this, type_map, descriptors};
    auto signature = complete_resolution(type_map, &wiring_arguments);
    return {std::move(signature), std::move(type_map)};
}

ResolvedSignature Signature::complete_resolution(TypeMap &type_map, const WiringArguments *arguments) const {
    auto resolver_context = ResolverContext{resolvers_, arguments, std::vector<ResolverState>(resolvers_.size())};
    type_map.missing_mapping_handler(&resolver_context, &resolve_missing_mapping);
    auto clear_handler = util::scope_exit{[&]() noexcept { type_map.missing_mapping_handler(nullptr, nullptr); }};

    for (const auto &resolver : resolvers_) {
        (void)type_map.find(*resolver.variable);
    }

    std::vector<ResolvedParameter> parameters;
    parameters.reserve(parameters_.size());

    for (const Parameter &parameter : parameters_) {
        const Type &type = parameter.type->resolve(type_map);

        if (!type.is_scalar() && !type.is_time_series() && !type.is_graph()) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, parameter.name,
                                      "parameter resolved outside its supported categories"};
        }

        parameters.push_back(
            {parameter.name, &type, parameter.activity, parameter.validity, parameter.accepts_absent_scalar,
             parameter.variadic_mode});
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

    return {name_, std::move(parameters), output};
}

} // namespace bg
