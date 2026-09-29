#include <bg/graph/context.hpp>

#include <bg/graph/details/inherited_time_series_section.hpp>
#include <bg/type/signature.hpp>

#include "thread_local_pointer.hpp"

namespace bg::details {

const char inherited_time_series_section_type{};

util::thread_local_pointer &ContextScope::active_scope() {
    static util::thread_local_pointer value;

    return value;
}

ContextScope::ContextScope(util::symbol name, WiringArgument argument)
    : name_{name}, argument_{std::move(argument)}, previous_{static_cast<ContextScope *>(active_scope().get())} {
    active_scope().set(this);
}

ContextScope::~ContextScope() {
    active_scope().set(previous_);
}

std::optional<WiringArgument> ContextScope::find(util::symbol name) {
    for (auto *scope = static_cast<ContextScope *>(active_scope().get()); scope != nullptr; scope = scope->previous_) {
        if (scope->name() == name) {
            return wiring_argument_access::borrow(scope->argument());
        }
    }
    return std::nullopt;
}

std::optional<WiringArgument> ContextScope::import(util::symbol name) {
    auto result = find(name);
    if (!result || !result->is_time_series()) {
        return result;
    }

    auto &wiring = WiringContext::current().wiring();
    const auto point = result->time_series();
    if (&point.graph() != &wiring) {
        wiring_argument_access::replace_time_series(*result, wiring.import_context(name, point));
    }
    return result;
}

ContextScopes::ContextScopes(const ResolvedSignature &signature, std::span<const WiringArgument> arguments) {
    scopes_.reserve(signature.parameters.size());
    try {
        for (auto index = std::size_t{}; index < signature.parameters.size(); ++index) {
            const auto &parameter = signature.parameters[index];
            if (parameter.context) {
                scopes_.push_back(
                    std::make_unique<ContextScope>(parameter.name, wiring_argument_access::borrow(arguments[index])));
            }
        }
    } catch (...) {
        while (!scopes_.empty()) {
            scopes_.pop_back();
        }
        throw;
    }
}

ContextScopes ContextScopes::activate_context_parameters(const ResolvedSignature &signature,
                                                         std::span<const WiringArgument> arguments) {
    return ContextScopes{signature, arguments};
}

ContextScopes::~ContextScopes() {
    while (!scopes_.empty()) {
        scopes_.pop_back();
    }
}

} // namespace bg::details
