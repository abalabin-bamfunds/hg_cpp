#pragma once

#include <hg/export.hpp>
#include <hg/type/type.hpp>
#include <hg/type/variable.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

namespace hg {

class HG_EXPORT VariableRegistry {
  public:
    VariableRegistry(const VariableRegistry &) = delete;
    VariableRegistry &operator=(const VariableRegistry &) = delete;

    static VariableRegistry &instance() noexcept;

    // The factory runs only on a miss and returns a new, unregistered process-static variable.
    template <typename Declaration, typename Descriptor, typename Factory>
        requires std::derived_from<Descriptor, Variable> && (!std::derived_from<Descriptor, Type>) &&
                 std::same_as<std::invoke_result_t<Factory>, const Descriptor &>
    const Descriptor &get_or_create_static(Factory &&factory) noexcept(std::is_nothrow_invocable_v<Factory>) {
        if (const auto *found = find_static_variable(typeid(Declaration), typeid(Descriptor))) {
            return static_cast<const Descriptor &>(*found);
        }

        const auto &variable = std::invoke(std::forward<Factory>(factory));
        register_static_variable(variable, typeid(Declaration), typeid(Descriptor));
        return variable;
    }

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

    bool contains(const Variable &variable) const;

  private:
    VariableRegistry() = default;
    static void register_static_variable(const Variable &variable) noexcept;
    void register_static_variable(const Variable &variable, const std::type_info &declaration,
                                  const std::type_info &descriptor);
    const Variable *find_static_variable(const std::type_info &declaration,
                                         const std::type_info &descriptor) const noexcept;
    void register_owned_variable(std::unique_ptr<Variable> variable);

    details::StaticRegistryEntry<Variable> *static_entries_{};
    std::vector<std::unique_ptr<Variable>> owned_variables_;
};

} // namespace hg
