#pragma once

#include <bg/node/input_properties.hpp>
#include <bg/graph/variadic_parameter.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <compare>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class WiringArguments;
class WiringArgument;

namespace details {
class WiringArgumentDescriptor;
class VariadicWiringOperations;
}

struct Parameter {
    util::symbol name;
    const Type *type;
    InputActivity activity{active};
    InputValidity validity{required};
    bool accepts_absent_scalar{};
    details::VariadicMode variadic_mode{details::VariadicMode::none};
    const details::VariadicWiringOperations *variadic_operations{};
};

struct VariableResolver {
    template <typename Function> VariableResolver(const Variable *variable, Function function) : variable{variable} {
        if constexpr (std::invocable<Function, const TypeMap &, const WiringArguments &>) {
            requires_arguments = true;
            resolve = [function = std::move(function)](const TypeMap &type_map, const WiringArguments *arguments) {
                return std::invoke(function, type_map, *arguments);
            };
        } else {
            static_assert(std::invocable<Function, const TypeMap &>);
            resolve = [function = std::move(function)](const TypeMap &type_map, const WiringArguments *) {
                return std::invoke(function, type_map);
            };
        }
    }

    const Variable *variable;
    std::function<TypeMapValue(const TypeMap &, const WiringArguments *)> resolve;
    bool requires_arguments{};
};

struct ResolvedParameter {
    util::symbol name;
    const Type *type;
    InputActivity activity;
    InputValidity validity;
    bool accepts_absent_scalar;
    details::VariadicMode variadic_mode;
};

struct ResolvedSignature {
    util::symbol name;
    std::vector<ResolvedParameter> parameters;
    const TimeSeriesType *output;
};

struct SignatureResolution {
    ResolvedSignature signature;
    TypeMap type_map;
};

class Signature {
  public:
    Signature(util::symbol name, std::vector<Parameter> parameters, std::vector<VariableResolver> resolvers = {})
        : name_(name), parameters_(std::move(parameters)), output_(nullptr), resolvers_(std::move(resolvers)) {
        validate_variadic_parameters();
    }

    Signature(util::symbol name, std::vector<Parameter> parameters, const TimeSeriesType &output,
              std::vector<VariableResolver> resolvers = {})
        : name_(name), parameters_(std::move(parameters)), output_(&output), resolvers_(std::move(resolvers)) {
        validate_variadic_parameters();
    }

    [[nodiscard]] ResolvedSignature resolve(std::span<const Type *const> arguments) const;
    [[nodiscard]] SignatureResolution resolve_with_type_map(std::span<const Type *const> arguments) const;
    [[nodiscard]] SignatureResolution resolve_with_type_map(std::span<const Type *const> arguments,
                                                            const TypeMap &type_map) const;
    [[nodiscard]] SignatureResolution resolve_wiring_arguments(
        std::span<const WiringArgument> arguments,
        std::span<const details::WiringArgumentDescriptor> descriptors,
        const TypeMap *type_map = nullptr) const;
    [[nodiscard]] TypeMap match_arguments(std::span<const Type *const> arguments,
                                          const TypeMap *type_map = nullptr) const;
    [[nodiscard]] ResolvedSignature complete_resolution(TypeMap &type_map,
                                                        const WiringArguments *arguments = nullptr) const;

    util::symbol name() const noexcept { return name_; }

    std::size_t parameter_count() const noexcept { return parameters_.size(); }

    const Parameter &parameter(std::size_t index) const noexcept { return parameters_[index]; }

    std::optional<std::size_t> variadic_parameter() const noexcept {
        if (parameters_.empty() || parameters_.back().variadic_mode == details::VariadicMode::none) {
            return std::nullopt;
        }
        return parameters_.size() - 1;
    }

    bool has_output() const noexcept { return output_ != nullptr; }
    const TimeSeriesType *output() const noexcept { return output_; }

  private:
    void validate_variadic_parameters() const;
    [[nodiscard]] SignatureResolution resolve_seeded(std::span<const Type *const> arguments, TypeMap type_map) const;

    util::symbol name_;
    std::vector<Parameter> parameters_;
    const TimeSeriesType *output_;
    std::vector<VariableResolver> resolvers_;
};

[[nodiscard]] std::partial_ordering compare_specificity(const Signature &left, const Signature &right);

namespace details {
[[nodiscard]] std::partial_ordering compare_parameter_specificity(const Signature &left, const Signature &right,
                                                                  std::size_t parameter_count);
}

} // namespace bg
