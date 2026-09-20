#pragma once

#include <bg/util/symbol.hpp>

#include <memory>
#include <optional>
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
    std::shared_ptr<const Type> type_owner;
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
};

struct WiringDiagnostic {
    util::symbol operator_name;
    std::vector<util::symbol> path;
    std::vector<WiringParameterDiagnostic> arguments;
    std::vector<WiringCandidateDiagnostic> candidates;
    bool ambiguous{};
};

class WiringError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;

    explicit WiringError(WiringDiagnostic diagnostic);

    const std::optional<WiringDiagnostic> &diagnostic() const noexcept { return diagnostic_; }

  private:
    static std::string render(const WiringDiagnostic &diagnostic);

    std::optional<WiringDiagnostic> diagnostic_;
};

} // namespace bg
