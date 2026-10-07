#include <bg/graph/context.hpp>

#include <bg/graph/details/inherited_time_series_section.hpp>
#include <bg/type/signature.hpp>

#include "thread_local_pointer.hpp"

#include <memory>
#include <vector>

namespace bg::details {

const char inherited_time_series_section_type{};

struct ContextSnapshot::Storage {
    std::vector<ContextBinding> bindings;
};

util::thread_local_pointer &ContextBindingState::active_binding() {
    static util::thread_local_pointer value;

    return value;
}

ContextBindingState::ContextBindingState(ContextBinding &binding) noexcept
    : previous_{current()} {
    binding.previous = previous_;
    active_binding().set(&binding);
}

ContextBindingState::ContextBindingState(const ContextBinding *head) noexcept
    : previous_{current()} {
    active_binding().set(const_cast<ContextBinding *>(head));
}

ContextBindingState::~ContextBindingState() {
    active_binding().set(const_cast<ContextBinding *>(previous_));
}

const ContextBinding *ContextBindingState::current() noexcept {
    return static_cast<const ContextBinding *>(active_binding().get());
}

std::optional<WiringArgument> ContextBindingState::find(util::symbol name) {
    for (auto *binding = current(); binding != nullptr; binding = binding->previous) {
        if (binding->name == name) {
            return WiringArgument::borrow(binding->argument);
        }
    }
    return std::nullopt;
}

ContextScope::ContextScope(util::symbol name, WiringArgument argument)
    : binding_{name, std::move(argument)}, state_{binding_} {}

ContextScope::~ContextScope() = default;

std::optional<WiringArgument> ContextScope::find(util::symbol name) {
    return ContextBindingState::find(name);
}

std::optional<WiringArgument> ContextScope::import(util::symbol name) {
    auto result = find(name);
    if (!result || !result->is_time_series()) {
        return result;
    }

    auto &wiring = WiringContext::current().wiring();
    const auto point = result->time_series();
    if (&point.graph() != &wiring) {
        result->replace_time_series(wiring.import_context(name, point));
    }
    return result;
}

ContextSnapshot::ContextSnapshot() = default;

ContextSnapshot::ContextSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_{std::move(storage)} {}

ContextSnapshot ContextSnapshot::capture() {
    auto binding_count = std::size_t{};
    for (auto *binding = ContextBindingState::current(); binding != nullptr; binding = binding->previous) {
        ++binding_count;
    }
    if (binding_count == 0) {
        return ContextSnapshot{};
    }

    auto storage = std::make_shared<Storage>();
    storage->bindings.reserve(binding_count);
    for (auto *binding = ContextBindingState::current(); binding != nullptr; binding = binding->previous) {
        storage->bindings.push_back({binding->name, WiringArgument{binding->argument}});
    }
    for (auto index = std::size_t{}; index + 1 < storage->bindings.size(); ++index) {
        storage->bindings[index].previous = &storage->bindings[index + 1];
    }
    return ContextSnapshot{std::move(storage)};
}

ContextSnapshot::Activation::Activation(const ContextSnapshot &snapshot) noexcept
    : storage_{snapshot.storage_}, state_{storage_ ? storage_->bindings.data() : nullptr} {}

ContextSnapshot::Activation::~Activation() = default;

ContextScopes::ContextScopes(const ResolvedSignature &signature, std::span<const WiringArgument> arguments) {
    scopes_.reserve(signature.parameters.size());
    try {
        for (auto index = std::size_t{}; index < signature.parameters.size(); ++index) {
            const auto &parameter = signature.parameters[index];
            if (parameter.context) {
                scopes_.push_back(
                    std::make_unique<ContextScope>(parameter.name, WiringArgument::borrow(arguments[index])));
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
