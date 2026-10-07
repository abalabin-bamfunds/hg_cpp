#pragma once

#include <bg/export.hpp>
#include <bg/type/type.hpp>
#include <bg/type/variable.hpp>

#include <atomic>
#include <concepts>
#include <memory>
#include <mutex>
#include <vector>

namespace bg {

class BG_EXPORT VariableRegistry {
  public:
    static VariableRegistry &instance() noexcept;

    template <typename T>
        requires std::derived_from<T, Variable> && (!std::derived_from<T, Type>)
    static const T &register_static(const T &variable) noexcept {
        register_static_variable(variable);
        return variable;
    }

    template <typename T>
        requires std::derived_from<T, Variable> && (!std::derived_from<T, Type>)
    const T &register_owned(std::unique_ptr<T> variable) {
        const auto *result = variable.get();
        register_owned_variable(std::move(variable));
        return *result;
    }

    void rollback(const Variable &variable) noexcept;

    bool contains(const Variable &variable) const;

  private:
    VariableRegistry() = default;
    static void register_static_variable(const Variable &variable) noexcept;
    void register_owned_variable(std::unique_ptr<Variable> variable);

    std::atomic<details::StaticRegistryEntry<Variable> *> static_entries_{};
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Variable>> owned_variables_;
};

} // namespace bg
