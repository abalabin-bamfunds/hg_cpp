#include <bg/type/signature.hpp>

namespace bg {

ResolvedSignature Signature::resolve(std::span<const Type *const> arguments) const {
    return std::move(resolve_with_bindings(arguments).signature);
}

SignatureResolution Signature::resolve_seeded(std::span<const Type *const> arguments, TypeMap bindings) const {
    if (arguments.size() != parameters_.size()) {
        throw TypeResolutionError{TypeResolutionErrorCode::argument_count, name_,
                                  "signature received the wrong number of arguments"};
    }

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto &parameter = parameters_[i];
        if (!parameter.type->match(*arguments[i], bindings)) {
            throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name,
                                      "argument does not satisfy its generic type"};
        }
    }

    return resolve(std::move(bindings));
}

SignatureResolution Signature::resolve_with_bindings(std::span<const Type *const> arguments) const {
    return resolve_seeded(arguments, TypeMap{});
}

SignatureResolution Signature::resolve_with_bindings(std::span<const Type *const> arguments,
                                                     const TypeMap &bindings) const {
    return resolve_seeded(arguments, bindings.copy());
}

SignatureResolution Signature::resolve(TypeMap bindings) const {
    for (const VariableResolver &resolver : resolvers_) {
        if (bindings.find(*resolver.variable) == nullptr) {
            bindings.set(*resolver.variable, resolver.resolve(bindings));
        }
    }

    std::vector<ResolvedParameter> parameters;
    parameters.reserve(parameters_.size());

    for (const Parameter &parameter : parameters_) {
        const Type &type = parameter.type->resolve(bindings);

        if (!type.is_scalar() && !type.is_time_series()) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, parameter.name,
                                      "parameter resolved outside its supported categories"};
        }

        parameters.push_back(
            {parameter.name, &type, parameter.activity, parameter.validity, parameter.accepts_absent_scalar});
    }

    const TimeSeriesType *output = nullptr;
    if (output_ != nullptr) {
        const Type &resolved_output = output_->resolve(bindings);
        if (!resolved_output.is_time_series()) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name_,
                                      "output did not resolve to a time series type"};
        }

        output = static_cast<const TimeSeriesType *>(&resolved_output);
    }

    return {{name_, std::move(parameters), output}, std::move(bindings)};
}

} // namespace bg
