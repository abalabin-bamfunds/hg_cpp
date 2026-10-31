#include <bg/type/variable_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>

namespace bg {

VariableRegistry &VariableRegistry::instance() noexcept {
    alignas(VariableRegistry) static std::byte storage[sizeof(VariableRegistry)];
    static auto *registry = ::new (static_cast<void *>(storage)) VariableRegistry{};
    return *registry;
}

void VariableRegistry::register_static_variable(const Variable &variable) noexcept {
    variable.registry_entry().link(instance().static_entries_);
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

} // namespace bg
