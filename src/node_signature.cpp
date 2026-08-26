#include <bg/type/node_signature.hpp>

namespace bg {

ResolvedNodeSignature NodeSignature::resolve(std::span<const Type *const> arguments) const {
    if (arguments.size() != parameters_.size()) {
        throw TypeResolutionError{TypeResolutionErrorCode::argument_count,
                                  name_,
                                  "node signature received the wrong number of arguments"};
    }

    TypeMap bindings;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (!parameters_[i].type->match(*arguments[i], bindings)) {
            throw TypeResolutionError{TypeResolutionErrorCode::mismatch,
                                      parameters_[i].name,
                                      "node argument does not satisfy its generic type"};
        }
    }

    for (const VariableResolver &resolver : resolvers_) {
        if (bindings.find(*resolver.variable) == nullptr) {
            bindings.set(*resolver.variable, resolver.resolve(bindings));
        }
    }

    std::vector<ResolvedNodeParameter> parameters;
    parameters.reserve(parameters_.size());

    for (const NodeParameter &parameter : parameters_) {
        const Type &type = parameter.type->resolve(bindings);

        if (!type.is_scalar() && !type.is_time_series()) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding,
                                      parameter.name,
                                      "node parameter resolved outside its supported categories"};
        }

        parameters.push_back({parameter.name, &type});
    }

    const Type &output = output_->resolve(bindings);
    if (!output.is_time_series()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding,
                                  name_,
                                  "node output did not resolve to a time series type"};
    }

    return {name_, std::move(parameters), static_cast<const TimeSeriesType *>(&output)};
}

} // namespace bg
