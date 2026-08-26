#pragma once

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <concepts>
#include <initializer_list>
#include <utility>
#include <vector>

namespace bg {

template <typename Category>
    requires std::derived_from<Category, Type>
class TypeVariable final : public Category, public Variable {
  public:
    explicit TypeVariable(util::symbol name, std::initializer_list<const Type *> constraints = {})
        : Variable(name), constraints_(constraints) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }

    [[nodiscard]] bool match(const Type &supplied, TypeMap &bindings) const override {
        if constexpr (!std::same_as<Category, Type>) {
            if (dynamic_cast<const Category *>(&supplied) == nullptr) {
                return false;
            }
        }

        if (const auto *binding = bindings.find(*this)) {
            const auto *type = std::get_if<const Type *>(binding);
            if (type == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding,
                                          name(),
                                          "type variable has a non-type binding"};
            }

            return *type == &supplied;
        }

        if (constraints_.empty()) {
            bindings.set(*this, &supplied);
            return true;
        }

        bool constraint_selected = false;
        TypeMap selected_bindings;

        for (const Type *constraint : constraints_) {
            TypeMap trial_bindings = bindings.overlay();

            if (!constraint->match(supplied, trial_bindings)) {
                continue;
            }

            if (constraint_selected) {
                throw TypeResolutionError{TypeResolutionErrorCode::ambiguous_constraint,
                                          name(),
                                          "more than one type constraint accepts the supplied "
                                          "type"};
            }

            constraint_selected = true;
            selected_bindings = std::move(trial_bindings);
        }

        if (!constraint_selected) {
            return false;
        }

        bindings.apply(std::move(selected_bindings));
        bindings.set(*this, &supplied);

        return true;
    }

    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override {
        const auto *binding = bindings.find(*this);
        if (binding == nullptr) {
            throw TypeResolutionError{
                TypeResolutionErrorCode::unbound_variable, name(), "type variable is unbound"};
        }

        const auto *type = std::get_if<const Type *>(binding);
        if (type == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding,
                                      name(),
                                      "type variable has a non-type binding"};
        }

        return **type;
    }

  private:
    std::vector<const Type *> constraints_;
};

} // namespace bg
