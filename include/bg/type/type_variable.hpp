#pragma once

#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <concepts>
#include <ranges>
#include <utility>
#include <vector>

namespace bg {

template <typename Category>
    requires std::derived_from<Category, Type>
class TypeVariable final : public Category, public Variable {
  public:
    using Constraint = bool (*)(const Category &) noexcept;

    explicit TypeVariable(util::symbol name, std::vector<const Category *> alternatives = {},
                          std::vector<Constraint> constraints = {})
        : Variable(name), alternatives_(std::move(alternatives)), constraints_(std::move(constraints)) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override {
        const Category *candidate;
        if constexpr (!std::same_as<Category, Type>) {
            candidate = dynamic_cast<const Category *>(&supplied);
            if (candidate == nullptr) {
                return false;
            }
        } else {
            candidate = &supplied;
        }

        if (!std::ranges::all_of(constraints_, [candidate](auto constraint) { return constraint(*candidate); })) {
            return false;
        }

        if (const auto *binding = bindings.find(*this)) {
            const auto *type = std::get_if<const Type *>(binding);
            if (type == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name(),
                                          "type variable has a non-type binding"};
            }

            return *type == &supplied;
        }

        if (alternatives_.empty()) {
            bindings.set(*this, &supplied);
            return true;
        }

        auto constraint_selected = false;
        auto selected_bindings = TypeMap{};

        for (const auto *alternative : alternatives_) {
            auto trial_bindings = bindings.overlay();

            if (!alternative->match(supplied, trial_bindings)) {
                continue;
            }

            if (constraint_selected) {
                throw TypeResolutionError{TypeResolutionErrorCode::ambiguous_constraint, name(),
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

  public:
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override {
        const auto *binding = bindings.find(*this);
        if (binding == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unbound_variable, name(), "type variable is unbound"};
        }

        const auto *type = std::get_if<const Type *>(binding);
        if (type == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name(),
                                      "type variable has a non-type binding"};
        }

        return **type;
    }

    [[nodiscard]] bool supports_equality() const noexcept {
        if constexpr (!std::same_as<Category, ScalarType>) {
            return false;
        } else {
            return std::ranges::contains(constraints_, &supports_scalar_equality) ||
                   (!alternatives_.empty() &&
                    std::ranges::all_of(alternatives_, [](const auto *type) { return type->supports_equality(); }));
        }
    }

    [[nodiscard]] bool supports_hash() const noexcept {
        if constexpr (!std::same_as<Category, ScalarType>) {
            return false;
        } else {
            return std::ranges::contains(constraints_, &supports_scalar_hash) ||
                   (!alternatives_.empty() &&
                    std::ranges::all_of(alternatives_, [](const auto *type) { return type->supports_hash(); }));
        }
    }

  private:
    std::vector<const Category *> alternatives_;
    std::vector<Constraint> constraints_;
};

} // namespace bg
