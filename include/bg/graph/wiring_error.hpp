#pragma once

#include <bg/node/input_property_types.hpp>
#include <bg/util/symbol.hpp>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg {

class Signature;
class Type;

enum class WiringFailureStage {
    argument_count,
    type_match,
    resolver,
    requirement,
    callback,
    applicable,
};

struct WiringParameterDiagnostic {
    util::symbol name;
    const Type *type;
    InputValidity validity{};
    bool valid_items{};
    struct Form {
        std::string type;
        double cost{};
        bool direct{};
    };
    std::vector<Form> offered_forms;
};

struct WiringBindingDiagnostic {
    std::size_t argument_index{};
    util::symbol parameter;
    std::string offered_type;
    std::string accepted_type;
    std::string materialisation_type;
    double cost{};
};

struct WiringSignatureDiagnostic {
    util::symbol name;
    std::vector<WiringParameterDiagnostic> parameters;
};

struct WiringCandidateDiagnostic {
    WiringSignatureDiagnostic signature;
    WiringFailureStage stage;
    std::optional<util::symbol> subject;
    const Type *expected{};
    const Type *supplied{};
    std::string message;
    std::vector<WiringBindingDiagnostic> bindings;
    std::optional<double> joint_materialisation_cost;
};

struct WiringDiagnostic {
    util::symbol operator_name;
    std::vector<util::symbol> path;
    std::vector<WiringParameterDiagnostic> arguments;
    std::vector<WiringCandidateDiagnostic> candidates;
    bool ambiguous{};
};

struct WiringErrorContext {
    WiringFailureStage stage{WiringFailureStage::callback};
    std::string path;
    util::symbol operation;
    std::optional<util::symbol> parameter;
    std::optional<std::uint32_t> item;
    std::string requested_type;
    std::string expected_type;
    std::string actual_type;

    bool operator==(const WiringErrorContext &) const = default;
};

struct WiringRequestOrigin {
    std::string path;
    util::symbol consumer;
    std::optional<util::symbol> parameter;
};

class WiringError : public std::runtime_error {
  public:
    explicit WiringError(std::string message, std::exception_ptr cause = {});

    explicit WiringError(WiringDiagnostic diagnostic, std::exception_ptr cause = {});

    const std::optional<WiringDiagnostic> &diagnostic() const noexcept { return diagnostic_; }
    std::span<const WiringErrorContext> contexts() const noexcept { return contexts_; }
    const std::exception_ptr &cause() const noexcept { return cause_; }

    WiringError with_context(WiringErrorContext context) const;
    static WiringError from_exception(std::string message, WiringErrorContext context,
                                      std::exception_ptr cause);
    static WiringError from_exception(std::string message, std::span<const WiringErrorContext> contexts,
                                      std::exception_ptr cause);

  private:
    WiringError(std::string message, std::optional<WiringDiagnostic> diagnostic,
                std::vector<WiringErrorContext> contexts, std::exception_ptr cause);
    static std::string render(const WiringDiagnostic &diagnostic);
    static std::string render(std::string message, std::span<const WiringErrorContext> contexts);

    std::string message_;
    std::optional<WiringDiagnostic> diagnostic_;
    std::vector<WiringErrorContext> contexts_;
    std::exception_ptr cause_;
};

} // namespace bg
