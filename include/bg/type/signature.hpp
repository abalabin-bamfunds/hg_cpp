#pragma once

#include <bg/graph/variadic_parameter.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <compare>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class GraphArgumentConstraint;
class WiringArguments;
class WiringArgument;
class WiringParameter;

namespace details {
class WiringArgumentDescriptor;
class VariadicWiringOperations;
} // namespace details

struct Parameter {
    util::symbol name;
    const Type *type;
    InputActivity activity{active};
    InputValidity validity{required};
    bool accepts_absent_scalar{};
    details::VariadicMode variadic_mode{details::VariadicMode::none};
    const details::VariadicWiringOperations *variadic_operations{};
    bool context{};
    bool dynamic_activity{};
    std::shared_ptr<const GraphArgumentConstraint> graph_constraint{};
    bool valid_items{};
    bool name_is_explicit{};
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
    InputActivity activity{active};
    InputValidity validity{required};
    bool accepts_absent_scalar{};
    details::VariadicMode variadic_mode{details::VariadicMode::none};
    bool context{};
    bool dynamic_activity{};
    bool valid_items{};
};

struct ResolvedSignature {
    util::symbol name;
    std::vector<ResolvedParameter> parameters;
    const TimeSeriesType *output;
};

struct SignatureResolution {
    ResolvedSignature signature;
    TypeMap type_map;
    // A null output is pending graph output inference when this is true; otherwise it is a sink.
    bool infer_output{};
};

struct WiringArgumentFormMatch {
    // Index of the original argument passed to Signature::match_wiring_arguments.
    std::size_t argument_index;
    const Type *offered_type;
    const TimeSeriesType *materialisation_target;
    double materialisation_cost;
};

struct WiringSignatureMatch {
    TypeMap type_map;
    std::vector<WiringArgumentFormMatch> argument_forms;
    double materialisation_cost{};
};

struct ResolvedWiringSignatureMatch {
    SignatureResolution resolution;
    std::vector<WiringArgumentFormMatch> argument_forms;
    double materialisation_cost{};
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
    [[nodiscard]] SignatureResolution
    resolve_wiring_arguments(std::span<const WiringArgument> arguments,
                             std::span<const details::WiringArgumentDescriptor> descriptors,
                             const TypeMap *type_map = nullptr,
                             const std::function<void(const TypeMap &)> &completion = {}) const;
    // Match normalized arguments, with one entry per signature parameter. Variadic captures remain one argument.
    // This returns match-only candidates and does not invoke materialisation builders or completion callbacks.
    [[nodiscard]] std::vector<WiringSignatureMatch>
    match_wiring_arguments(std::span<const WiringArgument> arguments, const TypeMap *type_map = nullptr) const;
    // Consume one form match and complete it while resolver lookup is active. The argument context supplies the
    // original descriptors and scalar-read tracking; resolved_type() observes this match's type map.
    [[nodiscard]] ResolvedWiringSignatureMatch complete_wiring_match(
        WiringSignatureMatch match, const WiringArguments &argument_context,
        const std::function<void(const TypeMap &)> &completion = {},
        std::vector<const Variable *> *observed_type_variables = nullptr,
        bool allow_inferred_output = false) const;
    [[nodiscard]] TypeMap match_arguments(std::span<const Type *const> arguments,
                                          const TypeMap *type_map = nullptr) const;
    void validate_wiring_arguments(std::span<const WiringArgument> arguments) const;
    // Completion runs after signature resolution while resolver lookup is active.
    // It must not retain that lookup scope.
    [[nodiscard]] ResolvedSignature complete_resolution(
        TypeMap &type_map, const WiringArguments *arguments = nullptr,
        const std::function<void(const TypeMap &)> &completion = {},
        std::vector<const Variable *> *observed_type_variables = nullptr) const;
    [[nodiscard]] ResolvedSignature complete_inferred_output(
        TypeMap &type_map, const TimeSeriesType &actual_output,
        const WiringArguments *arguments = nullptr) const;

    util::symbol name() const noexcept { return name_; }

    Signature with_name(util::symbol name) const {
        auto result = *this;
        result.name_ = name;
        return result;
    }

    Signature without_output() const {
        auto result = *this;
        result.output_ = nullptr;
        return result;
    }
    [[nodiscard]] Signature select_parameters(std::span<const std::size_t> indices) const;

    [[nodiscard]] Signature with_parameters(std::span<const WiringParameter> parameters) const;
    [[nodiscard]] Signature with_defaults(std::span<const WiringParameter> parameters) const;
    const WiringArgument *default_argument(std::size_t index) const noexcept;

    std::size_t parameter_count() const noexcept { return parameters_.size(); }

    const Parameter &parameter(std::size_t index) const noexcept { return parameters_[index]; }
    [[nodiscard]] bool is_union_parameter(std::size_t index) const noexcept;

    std::optional<std::size_t> variadic_parameter(details::VariadicMode mode) const noexcept {
        for (auto index = std::size_t{}; index < parameters_.size(); ++index) {
            if (parameters_[index].variadic_mode == mode) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::optional<std::size_t> first_variadic_parameter() const noexcept {
        for (auto index = std::size_t{}; index < parameters_.size(); ++index) {
            if (parameters_[index].variadic_mode != details::VariadicMode::none) {
                return index;
            }
        }
        return std::nullopt;
    }

    bool has_variadic_parameters() const noexcept { return first_variadic_parameter().has_value(); }

    bool has_output() const noexcept { return output_ != nullptr; }
    const TimeSeriesType *output() const noexcept { return output_; }

  private:
    void validate_variadic_parameters() const;
    void validate_graph_constraints(std::span<const WiringArgument> arguments) const;
    [[nodiscard]] SignatureResolution resolve_seeded(std::span<const Type *const> arguments, TypeMap type_map) const;
    [[nodiscard]] ResolvedSignature complete_resolution_impl(
        TypeMap &type_map, const WiringArguments *arguments,
        const std::function<void(const TypeMap &)> &completion,
        std::vector<const Variable *> *observed_type_variables, bool *infer_output) const;

    util::symbol name_;
    std::vector<Parameter> parameters_;
    const TimeSeriesType *output_;
    std::vector<VariableResolver> resolvers_;
    std::vector<std::shared_ptr<const WiringArgument>> defaults_;
};

[[nodiscard]] std::partial_ordering compare_specificity(const Signature &left, const Signature &right);
[[nodiscard]] bool same_resolved_signature(const ResolvedSignature &left, const ResolvedSignature &right) noexcept;

namespace details {
[[nodiscard]] std::partial_ordering compare_parameter_specificity(const Signature &left, const Signature &right,
                                                                  std::size_t parameter_count);
[[nodiscard]] std::partial_ordering compare_parameter_specificity(
    const Signature &left, const Signature &right, std::span<const Type *const> left_supplied_types,
    std::span<const Type *const> right_supplied_types, std::size_t parameter_count);
}

} // namespace bg
