#include <bg/graph/wiring_error.hpp>

#include <bg/type/type.hpp>

#include <cstddef>
#include <array>
#include <algorithm>
#include <utility>

namespace bg {
namespace {

void append_type(std::string &result, const Type *type) {
    if (type != nullptr) {
        type->format_name(result);
    }
}

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
        if (parameter.valid_items) {
            result += "valid_items ";
        }
        append_type(result, parameter.type);
    }
    result += ')';
}

std::string_view stage_name(WiringFailureStage stage) {
    switch (stage) {
    case WiringFailureStage::argument_count:
        return "argument count";
    case WiringFailureStage::type_match:
        return "type match";
    case WiringFailureStage::resolver:
        return "resolver";
    case WiringFailureStage::requirement:
        return "requirement";
    case WiringFailureStage::callback:
        return "callback";
    case WiringFailureStage::applicable:
        return "applicability";
    }
    return "wiring";
}

} // namespace

WiringError::WiringError(std::string message, std::exception_ptr cause)
    : WiringError{std::move(message), std::nullopt, {}, std::move(cause)} {}

WiringError::WiringError(WiringDiagnostic diagnostic, std::exception_ptr cause)
    : WiringError{render(diagnostic), std::optional<WiringDiagnostic>{std::move(diagnostic)}, {},
                  std::move(cause)} {}

WiringError::WiringError(std::string message, std::optional<WiringDiagnostic> diagnostic,
                         std::vector<WiringErrorContext> contexts, std::exception_ptr cause)
    : std::runtime_error(render(message, contexts)), message_{std::move(message)},
      diagnostic_{std::move(diagnostic)}, contexts_{std::move(contexts)}, cause_{std::move(cause)} {}

WiringError WiringError::with_context(WiringErrorContext context) const {
    auto contexts = contexts_;
    if (std::ranges::find(contexts, context) == contexts.end()) {
        contexts.push_back(std::move(context));
    }
    return WiringError{message_, diagnostic_, std::move(contexts), cause_};
}

WiringError WiringError::from_exception(std::string message, WiringErrorContext context,
                                        std::exception_ptr cause) {
    const auto contexts = std::array{std::move(context)};
    return from_exception(std::move(message), contexts, std::move(cause));
}

WiringError WiringError::from_exception(std::string message, std::span<const WiringErrorContext> contexts,
                                        std::exception_ptr cause) {
    if (cause == nullptr) {
        auto error = WiringError{std::move(message)};
        for (const auto &context : contexts) {
            error = error.with_context(context);
        }
        return error;
    }
    auto error = std::optional<WiringError>{};
    try {
        std::rethrow_exception(cause);
    } catch (const WiringError &error) {
        auto enriched = error;
        for (const auto &context : contexts) {
            enriched = enriched.with_context(context);
        }
        return enriched;
    } catch (const std::exception &error) {
        message += ": ";
        message += error.what();
    } catch (...) {
        message += ": non-standard exception";
    }
    error.emplace(std::move(message), std::move(cause));
    for (const auto &context : contexts) {
        *error = error->with_context(context);
    }
    return std::move(*error);
}

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
        if (diagnostic.arguments[index].valid_items) {
            result += "valid_items ";
        }
        append_type(result, diagnostic.arguments[index].type);
    }
    result += ')';

    const auto has_offered_forms = std::ranges::any_of(diagnostic.arguments, [](const auto &argument) {
        return !argument.offered_forms.empty();
    });
    if (has_offered_forms) {
        result += "\nOffered forms:";
        for (const auto &argument : diagnostic.arguments) {
            if (argument.offered_forms.empty()) {
                continue;
            }
            result += "\n  ";
            result += argument.name.text();
            result += ": ";
            for (auto index = std::size_t{}; index < argument.offered_forms.size(); ++index) {
                if (index != 0) {
                    result += ", ";
                }
                const auto &form = argument.offered_forms[index];
                result += form.type;
                result += form.direct ? " (direct)" : " (materialisation cost ";
                if (!form.direct) {
                    result += std::to_string(form.cost);
                    result += ')';
                }
            }
        }
    }

    for (const auto &candidate : diagnostic.candidates) {
        result += "\n\n";
        append_signature(result, candidate.signature);
        result += "\n  ";
        result += stage_name(candidate.stage);
        result += ": ";
        if (candidate.subject) {
            result += candidate.subject->text();
            result += ": ";
        }
        if (candidate.expected != nullptr && candidate.supplied != nullptr) {
            result += "expected ";
            append_type(result, candidate.expected);
            result += ", received ";
            append_type(result, candidate.supplied);
            if (!candidate.message.empty()) {
                result += ": ";
                result += candidate.message;
            }
        } else if (!candidate.message.empty()) {
            result += candidate.message;
        } else if (candidate.stage == WiringFailureStage::applicable) {
            result += "applicable";
        }
        for (const auto &binding : candidate.bindings) {
            result += "\n  argument ";
            result += std::to_string(binding.argument_index);
            result += " -> ";
            result += binding.parameter.text();
            result += " accepts ";
            result += binding.accepted_type;
            result += " from ";
            result += binding.offered_type;
            if (!binding.materialisation_type.empty()) {
                result += " materialised as ";
                result += binding.materialisation_type;
                result += " (cost ";
                result += std::to_string(binding.cost);
                result += ')';
            }
        }
        if (candidate.joint_materialisation_cost) {
            result += "\n  joint materialisation cost: ";
            result += std::to_string(*candidate.joint_materialisation_cost);
        }
    }
    return result;
}

std::string WiringError::render(std::string message, std::span<const WiringErrorContext> contexts) {
    for (const auto &context : contexts) {
        message += "\n  while ";
        message += stage_name(context.stage);
        message += ' ';
        if (context.operation.has_value()) {
            message += context.operation.text();
        } else {
            message += "wiring";
        }
        if (context.parameter) {
            message += " parameter '";
            message += context.parameter->text();
            message += '\'';
        }
        if (context.item) {
            message += " item ";
            message += std::to_string(*context.item);
        }
        if (!context.path.empty()) {
            message += " at ";
            message += context.path;
        }
        if (!context.requested_type.empty()) {
            message += " requesting ";
            message += context.requested_type;
        }
        if (!context.expected_type.empty() || !context.actual_type.empty()) {
            message += " (expected ";
            message += context.expected_type.empty() ? "?" : context.expected_type;
            message += ", received ";
            message += context.actual_type.empty() ? "?" : context.actual_type;
            message += ')';
        }
    }
    return message;
}

} // namespace bg
