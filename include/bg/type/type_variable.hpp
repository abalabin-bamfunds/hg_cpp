#pragma once

#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/compile_time_string.hpp>

#include <algorithm>
#include <concepts>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace bg {

// Engine and extension descriptor. Graph authors use ScalarVariable, TimeSeriesVariable, or TypeVariable declarations.
template <typename Category> class TypeVariableType final : public Category, public Variable {
    static_assert(std::derived_from<Category, Type>);

  public:
    using Constraint = bool (*)(const Category &) noexcept;
    using Category::match;

    explicit TypeVariableType(util::symbol name, std::vector<const Category *> alternatives = {},
                              std::vector<Constraint> constraints = {})
        : Variable(name), alternatives_(std::move(alternatives)), constraints_(std::move(constraints)) {}

    bool is_resolved() const noexcept override { return false; }
    void format_name(std::string &result) const override { result += name().text(); }

    // Alternatives may constrain the reference itself; constraints and mappings observe its limited target.
    [[nodiscard]] bool match(const Type &supplied, TypeMap &type_map,
                             std::optional<TimeSeriesGuarantee> reference_limit) const override {
        auto effective_limit = reference_limit;
        auto observed_type = &supplied;

        if (const auto *reference = type_cast<TimeSeriesReferenceType>(&supplied)) {
            if (!reference->is_resolved() || !reference->target_type().is_resolved()) {
                return false;
            }

            const auto argument_limit = reference->reference_limit();
            effective_limit = effective_limit ? weakest(*effective_limit, argument_limit) : argument_limit;
            observed_type = &reference->target_type();
        }

        if (effective_limit) {
            const auto *time_series = type_cast<TimeSeriesType>(observed_type);
            if (time_series != nullptr && time_series->is_resolved()) {
                observed_type = &time_series->with_guarantee_limit(*effective_limit);
            }
        }

        if constexpr (std::same_as<Category, TimeSeriesType>) {
            const auto *time_series = type_cast<TimeSeriesType>(observed_type);
            if (!type_map.matching_reference_target() && time_series != nullptr && time_series->is_resolved() &&
                time_series->contains_reference()) {
                observed_type = &time_series->reference_free_type();
            }
        }

        const Category *candidate;
        if constexpr (!std::same_as<Category, Type>) {
            candidate = dynamic_cast<const Category *>(observed_type);
            if (candidate == nullptr) {
                return false;
            }
        } else {
            candidate = observed_type;
        }

        if (!std::ranges::all_of(constraints_, [candidate](auto constraint) { return constraint(*candidate); })) {
            return false;
        }

        const auto *mapping = type_map.find(*this);
        if (mapping != nullptr) {
            const auto *type = std::get_if<const Type *>(mapping);
            if (type == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                          "type variable has a non-type mapping"};
            }

            if (*type != observed_type) {
                return false;
            }
        }

        if (alternatives_.empty()) {
            if (mapping == nullptr) {
                type_map.set(*this, observed_type);
            }
            return true;
        }

        auto alternative_matched = false;
        auto selected_type_map = TypeMap{};

        for (const auto *alternative : alternatives_) {
            auto trial_type_map = type_map.overlay();

            if (!alternative->match(supplied, trial_type_map, reference_limit)) {
                continue;
            }

            alternative_matched = true;
            if (!selected_type_map.apply_compatible(std::move(trial_type_map))) {
                throw TypeResolutionError{TypeResolutionErrorCode::ambiguous_constraint, name(),
                                          "matching alternatives produce conflicting type mappings"};
            }
        }

        if (!alternative_matched) {
            return false;
        }

        type_map.apply(std::move(selected_type_map));
        if (mapping == nullptr) {
            type_map.set(*this, observed_type);
        }

        return true;
    }

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

template <util::compile_time_string Name, typename... Alternatives> struct TypeVariable {
    static_assert(sizeof...(Alternatives) > 0, "a type variable requires at least one alternative");

    static const TypeVariableType<Type> &type() {
        static const auto variable = TypeVariableType<Type>{
            util::symbol{Name.view()}, std::vector<const Type *>{&alternative_type<Alternatives>()...}};
        [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(variable);

        return variable;
    }

    static constexpr auto name = Name;

  private:
    template <typename Alternative> static const Type &alternative_type() {
        if constexpr (requires { Alternative::type(); }) {
            return Alternative::type();
        } else {
            return cpp_scalar_type<Alternative>();
        }
    }
};

template <util::compile_time_string Name, typename... Alternatives>
using Union = TypeVariable<Name, Alternatives...>;

namespace details {

template <typename T> struct is_type_variable_declaration : std::false_type {};

template <util::compile_time_string Name, typename... Alternatives>
struct is_type_variable_declaration<TypeVariable<Name, Alternatives...>> : std::true_type {};

template <typename T>
concept type_variable_declaration = is_type_variable_declaration<std::remove_cv_t<T>>::value;

template <util::compile_time_string Name, typename... Alternatives>
struct declaration_variable_traits<TypeVariable<Name, Alternatives...>> {
    using Declaration = TypeVariable<Name, Alternatives...>;

    template <typename Visitor> static void for_each(Visitor &&visitor) {
        visitor.template operator()<Declaration>(static_cast<const Variable &>(Declaration::type()));
        (for_each_declaration_variable<Alternatives>(visitor), ...);
    }
};

} // namespace details

} // namespace bg
