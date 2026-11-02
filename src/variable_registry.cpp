#include <hg/type/variable_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>

namespace hg {

VariableRegistry &VariableRegistry::instance() noexcept {
    alignas(VariableRegistry) static std::byte storage[sizeof(VariableRegistry)];
    static auto *registry = ::new (static_cast<void *>(storage)) VariableRegistry{};
    return *registry;
}

void VariableRegistry::register_static_variable(const Variable &variable) noexcept {
    variable.registry_entry().link(instance().static_entries_);
}

void VariableRegistry::register_static_variable(const Variable &variable, const std::type_info &declaration,
                                                const std::type_info &descriptor) {
    if (!variable.registry_entry().link(static_entries_, declaration, descriptor)) {
        throw std::invalid_argument{"a static declaration factory returned a registered variable"};
    }
}

const Variable *VariableRegistry::find_static_variable(const std::type_info &declaration,
                                                       const std::type_info &descriptor) const noexcept {
    for (auto *entry = static_entries_; entry != nullptr; entry = entry->next()) {
        if (entry->matches(declaration, descriptor)) {
            return &entry->object();
        }
    }
    return nullptr;
}

void VariableRegistry::register_owned_variable(std::unique_ptr<Variable> variable) {
    owned_variables_.push_back(std::move(variable));
}

bool VariableRegistry::contains(const Variable &variable) const {
    for (auto *entry = static_entries_; entry != nullptr; entry = entry->next()) {
        if (&entry->object() == &variable) {
            return true;
        }
    }

    return std::ranges::any_of(owned_variables_, [&](const auto &candidate) { return candidate.get() == &variable; });
}

} // namespace hg
