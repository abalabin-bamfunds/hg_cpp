#include <bg/graph/wiring_error.hpp>

#include <bg/type/type_text.hpp>

#include <cstddef>
#include <utility>

namespace bg {
namespace {

void append_signature(std::string &result, const WiringSignatureDiagnostic &signature) {
    result += signature.name.text();
    result += '(';
    for (auto index = std::size_t{}; index < signature.parameters.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        const auto &parameter = signature.parameters[index];
        result += parameter.name.text();
        result += ": ";
        if (parameter.validity == InputValidity::optional) {
            result += "optional ";
        } else if (parameter.validity == InputValidity::all_valid) {
            result += "all_valid ";
        }
        append_type_text(result, *parameter.type);
    }
    result += ')';
}

} // namespace

WiringError::WiringError(WiringDiagnostic diagnostic)
    : std::runtime_error(render(diagnostic)), diagnostic_{std::move(diagnostic)} {}

std::string WiringError::render(const WiringDiagnostic &diagnostic) {
    auto result = std::string{diagnostic.ambiguous ? "Ambiguous operator " : "Cannot resolve operator "};
    result += diagnostic.operator_name.text();
    if (!diagnostic.path.empty()) {
        result += " at ";
        for (auto index = std::size_t{}; index < diagnostic.path.size(); ++index) {
            if (index != 0) {
                result += '/';
            }
            result += diagnostic.path[index].text();
        }
    }

    result += "\nArguments: (";
    for (auto index = std::size_t{}; index < diagnostic.arguments.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        result += diagnostic.arguments[index].name.text();
        result += ": ";
        if (diagnostic.arguments[index].validity == InputValidity::optional) {
            result += "opt ";
        }
        append_type_text(result, *diagnostic.arguments[index].type);
    }
    result += ')';

    for (const auto &candidate : diagnostic.candidates) {
        result += "\n\n";
        append_signature(result, candidate.signature);
        result += "\n  ";
        if (candidate.subject) {
            result += candidate.subject->text();
            result += ": ";
        }
        if (candidate.expected != nullptr && candidate.supplied != nullptr) {
            result += "expected ";
            append_type_text(result, *candidate.expected);
            result += ", received ";
            append_type_text(result, *candidate.supplied);
        } else if (!candidate.message.empty()) {
            result += candidate.message;
        } else if (candidate.stage == WiringFailureStage::applicable) {
            result += "applicable";
        }
    }
    return result;
}

} // namespace bg
