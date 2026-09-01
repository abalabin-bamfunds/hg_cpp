#pragma once

#include <bg/node/input_properties.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

struct Parameter {
    util::symbol name;
    const Type *type;
    InputActivity activity{active};
    InputValidity validity{required};
    bool accepts_absent_scalar{};
};

struct VariableResolver {
    const Variable *variable;
    std::function<TypeBinding(const TypeMap &)> resolve;
};

struct ResolvedParameter {
    util::symbol name;
    const Type *type;
    InputActivity activity;
    InputValidity validity;
    bool accepts_absent_scalar;
};

struct ResolvedSignature {
    util::symbol name;
    std::vector<ResolvedParameter> parameters;
    const TimeSeriesType *output;
};

struct SignatureResolution {
    ResolvedSignature signature;
    TypeMap bindings;
};

class Signature {
  public:
    Signature(util::symbol name, std::vector<Parameter> parameters, std::vector<VariableResolver> resolvers = {})
        : name_(name), parameters_(std::move(parameters)), output_(nullptr), resolvers_(std::move(resolvers)) {}

    Signature(util::symbol name, std::vector<Parameter> parameters, const TimeSeriesType &output,
              std::vector<VariableResolver> resolvers = {})
        : name_(name), parameters_(std::move(parameters)), output_(&output), resolvers_(std::move(resolvers)) {}

    [[nodiscard]] ResolvedSignature resolve(std::span<const Type *const> arguments) const;
    [[nodiscard]] SignatureResolution resolve_with_bindings(std::span<const Type *const> arguments) const;
    [[nodiscard]] SignatureResolution resolve_with_bindings(std::span<const Type *const> arguments,
                                                            const TypeMap &bindings) const;

    [[nodiscard]] util::symbol name() const noexcept { return name_; }

    [[nodiscard]] std::size_t parameter_count() const noexcept { return parameters_.size(); }

    [[nodiscard]] const Parameter &parameter(std::size_t index) const noexcept { return parameters_[index]; }

    [[nodiscard]] bool has_output() const noexcept { return output_ != nullptr; }

  private:
    [[nodiscard]] SignatureResolution resolve_seeded(std::span<const Type *const> arguments, TypeMap bindings) const;
    [[nodiscard]] SignatureResolution resolve(TypeMap bindings) const;

    util::symbol name_;
    std::vector<Parameter> parameters_;
    const TimeSeriesType *output_;
    std::vector<VariableResolver> resolvers_;
};

} // namespace bg
