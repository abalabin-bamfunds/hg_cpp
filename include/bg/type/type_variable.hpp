#pragma once

#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <concepts>
#include <ranges>
#include <span>
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

    bool is_resolved() const noexcept override { return false; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override {
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

        if (const auto *mapping = type_map.find(*this)) {
            const auto *type = std::get_if<const Type *>(mapping);
            if (type == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                          "type variable has a non-type mapping"};
            }

            return *type == &supplied;
        }

        if (alternatives_.empty()) {
            type_map.set(*this, &supplied);
            return true;
        }

        auto constraint_selected = false;
        auto selected_type_map = TypeMap{};

        for (const auto *alternative : alternatives_) {
            auto trial_type_map = type_map.overlay();

            if (!alternative->match(supplied, trial_type_map)) {
                continue;
            }

            if (constraint_selected) {
                throw TypeResolutionError{TypeResolutionErrorCode::ambiguous_constraint, name(),
                                          "more than one type constraint accepts the supplied "
                                          "type"};
            }

            constraint_selected = true;
            selected_type_map = std::move(trial_type_map);
        }

        if (!constraint_selected) {
            return false;
        }

        type_map.apply(std::move(selected_type_map));
        type_map.set(*this, &supplied);

        return true;
    }

  public:
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override {
        const auto *mapping = type_map.find(*this);
        if (mapping == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, name(),
                                      "type variable has no type map entry"};
        }

        const auto *type = std::get_if<const Type *>(mapping);
        if (type == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                      "type variable has a non-type mapping"};
        }

        return **type;
    }

    bool supports_equality() const noexcept {
        if constexpr (!std::same_as<Category, ScalarType>) {
            return false;
        } else {
            return std::ranges::contains(constraints_, &supports_scalar_equality) ||
                   (!alternatives_.empty() &&
                    std::ranges::all_of(alternatives_, [](const auto *type) { return type->supports_equality(); }));
        }
    }

    bool supports_hash() const noexcept {
        if constexpr (!std::same_as<Category, ScalarType>) {
            return false;
        } else {
            return std::ranges::contains(constraints_, &supports_scalar_hash) ||
                   (!alternatives_.empty() &&
                    std::ranges::all_of(alternatives_, [](const auto *type) { return type->supports_hash(); }));
        }
    }

    std::span<const Category *const> alternatives() const noexcept { return alternatives_; }
    std::span<const Constraint> constraints() const noexcept { return constraints_; }

  private:
    std::vector<const Category *> alternatives_;
    std::vector<Constraint> constraints_;
};

} // namespace bg
