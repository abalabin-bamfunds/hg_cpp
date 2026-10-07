#include <bg/type/variable_registry.hpp>

#include <algorithm>
#include <utility>

namespace bg {

VariableRegistry &VariableRegistry::instance() noexcept {
    static auto registry = VariableRegistry{};
    return registry;
}

void VariableRegistry::register_static_variable(const Variable &variable) noexcept {
    variable.registry_entry().link(instance().static_entries_);
}

void VariableRegistry::register_owned_variable(std::unique_ptr<Variable> variable) {
    const auto lock = std::lock_guard{mutex_};
    owned_variables_.push_back(std::move(variable));
}

void VariableRegistry::rollback(const Variable &variable) noexcept {
    auto removed = std::unique_ptr<Variable>{};
    {
        const auto lock = std::lock_guard{mutex_};
        const auto found = std::ranges::find(owned_variables_, &variable, [](const auto &candidate) {
            return candidate.get();
        });
        if (found == owned_variables_.end()) {
            return;
        }

        removed = std::move(*found);
        owned_variables_.erase(found);
    }
}

bool VariableRegistry::contains(const Variable &variable) const {
    for (auto *entry = static_entries_.load(std::memory_order_acquire); entry != nullptr; entry = entry->next()) {
        if (&entry->object() == &variable) {
            return true;
        }
    }

    const auto lock = std::lock_guard{mutex_};
    return std::ranges::any_of(owned_variables_, [&](const auto &candidate) { return candidate.get() == &variable; });
}

} // namespace bg
