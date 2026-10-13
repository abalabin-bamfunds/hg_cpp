#include <bg/type/signature.hpp>

#include <bg/graph/graph_argument.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/type/size_parameter.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/type_variable.hpp>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace bg {
namespace {

using Order = std::partial_ordering;

enum class ProjectionChild : std::uint8_t {
    graph_parameter,
    graph_output,
    list_item,
    set_item,
    dict_key,
    dict_value,
    scalar_type,
    time_series_value,
    time_series_list_item,
    time_series_set_item,
    time_series_dict_key,
    time_series_dict_value,
    time_series_bundle_schema
};

// Keeps repeated type, guarantee, and size declarations related across a signature.
struct DeclarationIdentity {
    const Variable *variable{};
    const Type *type{};
    std::optional<std::size_t> size_value;
    std::optional<TimeSeriesGuarantee> guarantee_value;
    std::vector<std::pair<ProjectionChild, std::size_t>> child_path;

    bool operator==(const DeclarationIdentity &) const = default;
};

// Preserves a union's original alternatives and variable identity during nested comparison.
struct Projection {
    const Variable *origin{};
    std::vector<std::pair<ProjectionChild, std::size_t>> child_path;
    std::vector<const Type *> alternatives;

    explicit operator bool() const noexcept { return origin != nullptr; }
};

Projection reference_target_projection(const Projection &projection);

struct Comparison {
    Order order{Order::equivalent};
    bool collect_declarations{true};
    std::vector<std::pair<DeclarationIdentity, DeclarationIdentity>> declarations;

    void record_declaration_pair(DeclarationIdentity left, DeclarationIdentity right) {
        if (collect_declarations && (left.variable != nullptr || right.variable != nullptr)) {
            declarations.emplace_back(std::move(left), std::move(right));
        }
    }
};

Order combine(Order left, Order right) noexcept {
    if (left == Order::unordered || right == Order::unordered) {
        return Order::unordered;
    }
    if (left == Order::equivalent) {
        return right;
    }
    if (right == Order::equivalent || left == right) {
        return left;
    }
    return Order::unordered;
}

Order compare_types(const Type &left, const Type &right, Comparison &comparison,
                    const Type *left_supplied = nullptr, const Type *right_supplied = nullptr,
                    Projection left_projection = {}, Projection right_projection = {});

template <typename TypeVariable> struct type_variable_category;

template <typename Category> struct type_variable_category<TypeVariableType<Category>> {
    using type = Category;
};

template <typename Function> auto visit_variable(const Type &type, Function &&function) {
    using Result = std::invoke_result_t<Function, const TypeVariableType<Type> &>;
    if (const auto *variable = dynamic_cast<const TypeVariableType<Type> *>(&type)) {
        return std::optional<Result>{std::invoke(function, *variable)};
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<ScalarType> *>(&type)) {
        return std::optional<Result>{std::invoke(function, *variable)};
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<TimeSeriesType> *>(&type)) {
        return std::optional<Result>{std::invoke(function, *variable)};
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<SchemaType> *>(&type)) {
        return std::optional<Result>{std::invoke(function, *variable)};
    }
    return std::optional<Result>{};
}

DeclarationIdentity declaration_identity(const Type &type, const Projection &projection) {
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&type)) {
        return declaration_identity(reference->target_type(), reference_target_projection(projection));
    }

    if (const auto variable = visit_variable(type, [](const auto &value) -> const Variable * {
            return std::addressof(value);
        })) {
        return {.variable = *variable};
    }
    if (const auto *series_variable = dynamic_cast<const GenericTimeSeriesVariableType *>(&type)) {
        return {.variable = std::addressof(series_variable->variable())};
    }
    if (projection) {
        return {.variable = projection.origin, .child_path = projection.child_path};
    }
    return {.type = std::addressof(type)};
}

DeclarationIdentity declaration_identity(const SizeParameter &size) {
    return {.variable = size.variable(), .size_value = size.value()};
}

DeclarationIdentity declaration_identity(const GuaranteeParameter &guarantee) {
    return {.variable = guarantee.variable(), .guarantee_value = guarantee.value()};
}

Projection projection_for(const Type &type) {
    if (const auto result = visit_variable(type, [&](const auto &variable) {
        const auto &alternatives = variable.alternatives();
        if (alternatives.size() > 1 && !std::ranges::all_of(alternatives, [&](const auto *alternative) {
                return alternative == alternatives.front();
            })) {
            auto projected_alternatives = std::vector<const Type *>{};
            projected_alternatives.reserve(alternatives.size());
            for (const auto *alternative : alternatives) {
                projected_alternatives.push_back(alternative);
            }
            return Projection{std::addressof(variable), {}, std::move(projected_alternatives)};
        }
        return Projection{};
    })) {
        return *result;
    }
    return {};
}

template <typename Parent> const Parent *projection_parent(const Type *type) {
    if constexpr (std::same_as<Parent, TypeScalarType>) {
        return dynamic_cast<const Parent *>(type);
    } else {
        return type_cast<Parent>(type);
    }
}

template <typename Parent, typename Getter>
Projection child_projection(const Projection &parent, const Type &selected_parent, ProjectionChild child,
                            std::size_t index, Getter &&getter) {
    if (!parent) {
        return {};
    }

    auto child_alternatives = std::vector<const Type *>{};
    for (const auto *alternative : parent.alternatives) {
        const auto *typed_alternative = projection_parent<Parent>(alternative);
        if (typed_alternative == nullptr) {
            continue;
        }

        if constexpr (std::same_as<Parent, GraphArgumentType>) {
            const auto *selected_graph = projection_parent<Parent>(&selected_parent);
            if (selected_graph == nullptr || typed_alternative->has_signature() != selected_graph->has_signature() ||
                typed_alternative->parameters().size() != selected_graph->parameters().size() ||
                (typed_alternative->output() == nullptr) != (selected_graph->output() == nullptr)) {
                continue;
            }
            if (child == ProjectionChild::graph_parameter && index >= typed_alternative->parameters().size()) {
                continue;
            }
        }

        child_alternatives.push_back(std::addressof(std::invoke(getter, *typed_alternative)));
    }
    if (child_alternatives.size() < 2 ||
        std::ranges::all_of(child_alternatives, [&](const auto *child) { return child == child_alternatives.front(); })) {
        return {};
    }

    auto result = Projection{parent.origin, parent.child_path, std::move(child_alternatives)};
    result.child_path.emplace_back(child, index);
    return result;
}

std::optional<std::vector<const Type *>> applicable_alternatives(const Type &pattern, const Type *supplied) {
    if (supplied == nullptr || !supplied->is_resolved()) {
        return std::nullopt;
    }

    const auto result = visit_variable(pattern, [&](const auto &variable) -> std::optional<std::vector<const Type *>> {
        if (variable.alternatives().empty()) {
            return std::nullopt;
        }

        if (!variable.accepts(*supplied)) {
            return std::nullopt;
        }

        auto matching = std::vector<const Type *>{};
        for (const auto *alternative : variable.alternatives()) {
            if (alternative->accepts(*supplied)) {
                matching.push_back(alternative);
            }
        }
        return matching.empty() ? std::nullopt
                                : std::optional<std::vector<const Type *>>{std::move(matching)};
    });
    return result ? *result : std::nullopt;
}

template <typename Category>
bool pattern_satisfies_base(const Type &pattern,
                            std::span<const typename TypeVariableType<Category>::Constraint> constraints,
                            Comparison &comparison);

template <typename Category>
bool pattern_rejected_by_base(const Type &pattern,
                              std::span<const typename TypeVariableType<Category>::Constraint> constraints);

template <typename LeftCategory, typename RightCategory>
bool variable_subset(const TypeVariableType<LeftCategory> &left, const TypeVariableType<RightCategory> &right,
                     Comparison &comparison);

Order compare_guarantees(const GuaranteeParameter &left, const GuaranteeParameter &right, Comparison &comparison);
Order compare_sizes(const SizeParameter &left, const SizeParameter &right, Comparison &comparison);

template <typename LeftConstraints, typename RightConstraints>
bool constraints_include(LeftConstraints left, RightConstraints right) {
    return std::ranges::all_of(right, [left](auto constraint) {
        return std::ranges::find(left, constraint) != left.end();
    });
}

template <typename SourceCategory, typename TargetCategory>
bool variable_satisfies_base(const TypeVariableType<SourceCategory> &variable,
                             std::span<const typename TypeVariableType<TargetCategory>::Constraint> constraints,
                             Comparison &comparison) {
    if (variable.alternatives().empty()) {
        if constexpr (!std::derived_from<SourceCategory, TargetCategory>) {
            return false;
        } else if constexpr (std::same_as<SourceCategory, TargetCategory>) {
            return constraints_include(variable.constraints(), constraints);
        } else {
            return constraints.empty();
        }
    }

    auto has_possible_alternative = false;
    return std::ranges::all_of(variable.alternatives(), [&](const auto *alternative) {
        if (pattern_rejected_by_base<SourceCategory>(*alternative, variable.constraints())) {
            return true;
        }

        auto trial = comparison;
        if (!pattern_satisfies_base<SourceCategory>(*alternative, variable.constraints(), trial)) {
            return false;
        }
        has_possible_alternative = true;
        if (!pattern_satisfies_base<TargetCategory>(*alternative, constraints, trial)) {
            return false;
        }
        comparison = std::move(trial);
        return true;
    }) && has_possible_alternative;
}

template <typename Category>
bool pattern_satisfies_base(const Type &pattern,
                            std::span<const typename TypeVariableType<Category>::Constraint> constraints,
                            Comparison &comparison) {
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&pattern)) {
        return pattern_satisfies_base<Category>(reference->target_type(), constraints, comparison);
    }

    if (const auto variable = visit_variable(pattern, [&](const auto &candidate) -> bool {
            using Candidate = std::remove_cvref_t<decltype(candidate)>;
            using CandidateCategory = typename type_variable_category<Candidate>::type;
            return variable_satisfies_base<CandidateCategory, Category>(candidate, constraints, comparison);
        })) {
        return *variable;
    }

    const auto *category = dynamic_cast<const Category *>(&pattern);
    return category != nullptr && std::ranges::all_of(constraints, [category](auto constraint) {
               return constraint(*category);
    });
}

template <typename Category>
bool pattern_rejected_by_base(const Type &pattern,
                              std::span<const typename TypeVariableType<Category>::Constraint> constraints) {
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&pattern)) {
        return pattern_rejected_by_base<Category>(reference->target_type(), constraints);
    }

    if (const auto variable = visit_variable(pattern, [&](const auto &candidate) {
            using Candidate = std::remove_cvref_t<decltype(candidate)>;
            using CandidateCategory = typename type_variable_category<Candidate>::type;
            if (candidate.alternatives().empty()) {
                return !std::derived_from<CandidateCategory, Category>;
            }
            return std::ranges::all_of(candidate.alternatives(), [&](const auto *alternative) {
                return pattern_rejected_by_base<Category>(*alternative, constraints);
            });
        })) {
        return *variable;
    }

    const auto *category = dynamic_cast<const Category *>(&pattern);
    if (category == nullptr) {
        return pattern.is_resolved();
    }
    return pattern.is_resolved() && std::ranges::any_of(constraints, [category](auto constraint) {
               return !constraint(*category);
           });
}

template <typename LeftCategory, typename RightCategory>
bool variable_subset(const TypeVariableType<LeftCategory> &left, const TypeVariableType<RightCategory> &right,
                     Comparison &comparison) {
    if (left.alternatives().empty()) {
        auto trial = comparison;
        if (!pattern_satisfies_base<RightCategory>(left, right.constraints(), trial)) {
            return false;
        }
        if (right.alternatives().empty()) {
            comparison = std::move(trial);
            return true;
        }
        return std::ranges::any_of(right.alternatives(), [&](const auto *alternative) {
            auto alternative_trial = trial;
            const auto order = compare_types(left, *alternative, alternative_trial);
            if (order != Order::less && order != Order::equivalent) {
                return false;
            }
            comparison = std::move(alternative_trial);
            return true;
        });
    }

    auto has_possible_alternative = false;
    return std::ranges::all_of(left.alternatives(), [&](const auto *alternative) {
        if (pattern_rejected_by_base<LeftCategory>(*alternative, left.constraints())) {
            return true;
        }

        auto trial = comparison;
        if (!pattern_satisfies_base<LeftCategory>(*alternative, left.constraints(), trial)) {
            return false;
        }
        has_possible_alternative = true;
        if (!pattern_satisfies_base<RightCategory>(*alternative, right.constraints(), trial)) {
            return false;
        }
        if (!right.alternatives().empty() &&
            !std::ranges::any_of(right.alternatives(), [&](const auto *right_alternative) {
                auto alternative_trial = trial;
                const auto order = compare_types(*alternative, *right_alternative, alternative_trial);
                if (order != Order::less && order != Order::equivalent) {
                    return false;
                }
                trial = std::move(alternative_trial);
                return true;
            })) {
            return false;
        }
        comparison = std::move(trial);
        return true;
    }) && has_possible_alternative;
}

template <typename LeftCategory, typename RightCategory>
Order compare_variables(const TypeVariableType<LeftCategory> &left, const TypeVariableType<RightCategory> &right,
                        Comparison &comparison) {
    auto left_trial = comparison;
    auto right_trial = comparison;
    left_trial.collect_declarations = false;
    right_trial.collect_declarations = false;
    const auto left_subset = variable_subset(left, right, left_trial);
    const auto right_subset = variable_subset(right, left, right_trial);
    if (left_subset && right_subset) {
        return Order::equivalent;
    }
    if (left_subset) {
        return Order::less;
    }
    if (right_subset) {
        return Order::greater;
    }
    return Order::unordered;
}

bool variable_accepts(const Type &variable, const Type &type, Comparison &comparison) {
    auto trial = comparison;
    trial.collect_declarations = false;
    const auto result = visit_variable(variable, [&](const auto &descriptor) {
        using Descriptor = std::remove_cvref_t<decltype(descriptor)>;
        using Category = typename type_variable_category<Descriptor>::type;
        return pattern_satisfies_base<Category>(type, descriptor.constraints(), trial) &&
               (descriptor.alternatives().empty() ||
                std::ranges::any_of(descriptor.alternatives(), [&](const auto *alternative) {
                    auto alternative_trial = trial;
                    const auto order = compare_types(type, *alternative, alternative_trial);
                    if (order != Order::less && order != Order::equivalent) {
                        return false;
                    }
                    trial = std::move(alternative_trial);
                    return true;
                }));
    });
    if (!result.value_or(false)) {
        return false;
    }

    return true;
}

Order compare_guarantees(const GuaranteeParameter &left, const GuaranteeParameter &right, Comparison &comparison) {
    comparison.record_declaration_pair(declaration_identity(left), declaration_identity(right));

    const auto left_value = left.value();
    const auto right_value = right.value();
    if (left_value && right_value) {
        return *left_value <=> *right_value == 0 ? Order::equivalent
                                                 : (*left_value > *right_value ? Order::less : Order::greater);
    }
    if (left_value) {
        return Order::less;
    }
    if (right_value) {
        return Order::greater;
    }

    const auto *left_variable = left.variable();
    const auto *right_variable = right.variable();
    if (left_variable == nullptr || right_variable == nullptr) {
        if (left_variable == right_variable) {
            return Order::equivalent;
        }
        return left_variable == nullptr ? Order::greater : Order::less;
    }

    const auto left_minimum = left_variable->minimum();
    const auto right_minimum = right_variable->minimum();
    if (left_minimum == right_minimum) {
        return Order::equivalent;
    }
    if (!left_minimum) {
        return Order::greater;
    }
    if (!right_minimum) {
        return Order::less;
    }
    return *left_minimum > *right_minimum ? Order::less : Order::greater;
}

Order compare_sizes(const SizeParameter &left, const SizeParameter &right, Comparison &comparison) {
    comparison.record_declaration_pair(declaration_identity(left), declaration_identity(right));

    const auto left_value = left.value();
    const auto right_value = right.value();
    if (left_value && right_value) {
        return *left_value == *right_value ? Order::equivalent : Order::unordered;
    }
    if (left_value) {
        return Order::less;
    }
    if (right_value) {
        return Order::greater;
    }

    return Order::equivalent;
}

Order compare_resolved(const Type &left, const Type &right) {
    if (!left.is_resolved() || !right.is_resolved()) {
        return Order::unordered;
    }

    const auto right_accepts_left = right.accepts(left);
    const auto left_accepts_right = left.accepts(right);
    if (right_accepts_left && left_accepts_right) {
        return Order::equivalent;
    }
    if (right_accepts_left) {
        return Order::less;
    }
    if (left_accepts_right) {
        return Order::greater;
    }
    return Order::unordered;
}

std::optional<Order> compare_contextual_alternatives(const Type &left, const Type &right, Comparison &comparison,
                                                     const Type *left_supplied, const Type *right_supplied,
                                                     const Projection &left_projection,
                                                     const Projection &right_projection) {
    std::optional<std::vector<const Type *>> left_alternatives;
    std::optional<std::vector<const Type *>> right_alternatives;
    try {
        left_alternatives = applicable_alternatives(left, left_supplied);
        right_alternatives = applicable_alternatives(right, right_supplied);
    } catch (const TypeResolutionError &) {
        return std::nullopt;
    }
    if (!left_alternatives && !right_alternatives) {
        return std::nullopt;
    }

    auto left_patterns = left_alternatives.value_or(std::vector<const Type *>{&left});
    auto right_patterns = right_alternatives.value_or(std::vector<const Type *>{&right});
    auto result = Order::equivalent;
    const auto declaration_count = comparison.declarations.size();
    auto common_declarations = std::vector<std::pair<DeclarationIdentity, DeclarationIdentity>>{};
    auto first_combination = true;
    for (const auto *left_pattern : left_patterns) {
        for (const auto *right_pattern : right_patterns) {
            auto trial = comparison;
            auto left_origin = left_alternatives ? projection_for(left) : left_projection;
            auto right_origin = right_alternatives ? projection_for(right) : right_projection;
            const auto order = compare_types(*left_pattern, *right_pattern, trial, left_supplied, right_supplied,
                                             left_origin, right_origin);
            result = combine(result, order);
            auto branch_declarations = std::vector<std::pair<DeclarationIdentity, DeclarationIdentity>>{
                trial.declarations.begin() + static_cast<std::ptrdiff_t>(declaration_count), trial.declarations.end()};
            if (first_combination) {
                common_declarations = std::move(branch_declarations);
                first_combination = false;
            } else {
                std::erase_if(common_declarations, [&](const auto &declaration) {
                    return std::ranges::find(branch_declarations, declaration) == branch_declarations.end();
                });
            }
        }
    }

    if (result == Order::equivalent) {
        return compare_types(left, right, comparison);
    }
    if (comparison.collect_declarations) {
        comparison.declarations.insert(comparison.declarations.end(), common_declarations.begin(),
                                       common_declarations.end());
    }

    return result;
}

const Type *context_for_child(const Type &declared_parent, const Type *supplied) {
    const auto *reference = type_cast<TimeSeriesReferenceType>(supplied);
    if (reference == nullptr) {
        return supplied;
    }

    if (type_cast<TimeSeriesReferenceType>(&declared_parent) != nullptr) {
        return &reference->target_type();
    }
    if (!reference->is_resolved() || !reference->target_type().is_resolved()) {
        return nullptr;
    }

    try {
        return &reference->dereferenced_type();
    } catch (const TypeResolutionError &) {
        return nullptr;
    }
}

template <typename Parent, typename Getter>
Order compare_child(const Type &left_parent, const Type &right_parent, Comparison &comparison,
                    const Type *left_supplied, const Type *right_supplied, const Projection &left_projection,
                    const Projection &right_projection, ProjectionChild child, std::size_t index, Getter getter) {
    const auto *left = projection_parent<Parent>(&left_parent);
    const auto *right = projection_parent<Parent>(&right_parent);
    if (left == nullptr || right == nullptr) {
        return Order::unordered;
    }

    const auto *left_context =
        projection_parent<Parent>(context_for_child(left_parent, left_supplied));
    const auto *right_context =
        projection_parent<Parent>(context_for_child(right_parent, right_supplied));
    const auto *left_supplied_child =
        left_context == nullptr ? nullptr : std::addressof(std::invoke(getter, *left_context));
    const auto *right_supplied_child =
        right_context == nullptr ? nullptr : std::addressof(std::invoke(getter, *right_context));
    return compare_types(std::invoke(getter, *left), std::invoke(getter, *right), comparison, left_supplied_child,
                         right_supplied_child,
                         child_projection<Parent>(left_projection, *left, child, index, getter),
                         child_projection<Parent>(right_projection, *right, child, index, getter));
}

Projection reference_target_projection(const Projection &projection) {
    if (!projection) {
        return {};
    }

    auto result = Projection{projection.origin, projection.child_path, {}};
    result.alternatives.reserve(projection.alternatives.size());
    for (const auto *alternative : projection.alternatives) {
        const auto *reference = type_cast<TimeSeriesReferenceType>(alternative);
        result.alternatives.push_back(reference == nullptr ? alternative : &reference->target_type());
    }
    return result;
}

Order compare_types(const Type &left, const Type &right, Comparison &comparison, const Type *left_supplied,
                    const Type *right_supplied, Projection left_projection, Projection right_projection) {
    comparison.record_declaration_pair(declaration_identity(left, left_projection),
                                       declaration_identity(right, right_projection));

    if (const auto contextual = compare_contextual_alternatives(left, right, comparison, left_supplied,
                                                                right_supplied, left_projection, right_projection)) {
        return *contextual;
    }

    const auto *left_reference = type_cast<TimeSeriesReferenceType>(&left);
    const auto *right_reference = type_cast<TimeSeriesReferenceType>(&right);
    if (left_reference != nullptr || right_reference != nullptr) {
        const auto &left_target = left_reference == nullptr ? left : left_reference->target_type();
        const auto &right_target = right_reference == nullptr ? right : right_reference->target_type();
        return compare_types(left_target, right_target, comparison,
                             context_for_child(left, left_supplied), context_for_child(right, right_supplied),
                             left_reference == nullptr ? left_projection
                                                       : reference_target_projection(left_projection),
                             right_reference == nullptr ? right_projection
                                                        : reference_target_projection(right_projection));
    }

    if (const auto result = visit_variable(left, [&](const auto &left_variable) -> Order {
            if (const auto right_variable = visit_variable(right, [&](const auto &right_variable) {
                    return compare_variables(left_variable, right_variable, comparison);
                })) {
                return *right_variable;
            }
            return variable_accepts(left, right, comparison) ? Order::greater : Order::unordered;
        })) {
        return *result;
    }
    if (const auto result = visit_variable(right, [&](const auto &) -> Order {
            return variable_accepts(right, left, comparison) ? Order::less : Order::unordered;
        })) {
        return *result;
    }

    const auto *left_series_variable = dynamic_cast<const GenericTimeSeriesVariableType *>(&left);
    const auto *right_series_variable = dynamic_cast<const GenericTimeSeriesVariableType *>(&right);
    if (left_series_variable != nullptr && right_series_variable != nullptr) {
        auto result =
            compare_variables(left_series_variable->variable(), right_series_variable->variable(), comparison);
        return combine(result, compare_guarantees(left_series_variable->guarantee(), right_series_variable->guarantee(),
                                                  comparison));
    }
    if (left_series_variable != nullptr || right_series_variable != nullptr) {
        const auto &variable =
            left_series_variable == nullptr ? right_series_variable->variable() : left_series_variable->variable();
        const auto &other = left_series_variable == nullptr ? left : right;
        if (!variable_accepts(variable, other, comparison)) {
            return Order::unordered;
        }
        return left_series_variable == nullptr ? Order::less : Order::greater;
    }

    if (const auto *left_graph = type_cast<GraphArgumentType>(&left)) {
        const auto *right_graph = type_cast<GraphArgumentType>(&right);
        if (right_graph == nullptr) {
            return Order::unordered;
        }
        if (left_graph->has_signature() != right_graph->has_signature()) {
            return left_graph->has_signature() ? Order::less : Order::greater;
        }
        if (!left_graph->has_signature()) {
            return Order::equivalent;
        }
        if (left_graph->parameters().size() != right_graph->parameters().size() ||
            (left_graph->output() == nullptr) != (right_graph->output() == nullptr)) {
            return Order::unordered;
        }

        auto result = Order::equivalent;
        for (auto index = std::size_t{}; index < left_graph->parameters().size(); ++index) {
            result = combine(result, compare_child<GraphArgumentType>(
                                         left, right, comparison, left_supplied, right_supplied, left_projection,
                                         right_projection, ProjectionChild::graph_parameter, index,
                                         [index](const auto &graph) -> const Type & {
                                             return *graph.parameters()[index];
                                         }));
        }
        if (left_graph->output() != nullptr) {
            result = combine(result, compare_child<GraphArgumentType>(
                                         left, right, comparison, left_supplied, right_supplied, left_projection,
                                         right_projection, ProjectionChild::graph_output, 0,
                                         [](const auto &graph) -> const Type & { return *graph.output(); }));
        }
        return result;
    }

    if (type_cast<ListScalarType>(&left) != nullptr && type_cast<ListScalarType>(&right) != nullptr) {
        return compare_child<ListScalarType>(left, right, comparison, left_supplied, right_supplied,
                                             left_projection, right_projection, ProjectionChild::list_item, 0,
                                             &ListScalarType::item_type);
    }
    if (type_cast<SetScalarType>(&left) != nullptr && type_cast<SetScalarType>(&right) != nullptr) {
        return compare_child<SetScalarType>(left, right, comparison, left_supplied, right_supplied,
                                            left_projection, right_projection, ProjectionChild::set_item, 0,
                                            &SetScalarType::item_type);
    }
    if (type_cast<DictScalarType>(&left) != nullptr && type_cast<DictScalarType>(&right) != nullptr) {
        auto result = compare_child<DictScalarType>(
            left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
            ProjectionChild::dict_key, 0, &DictScalarType::key_type);
        return combine(result, compare_child<DictScalarType>(
                                   left, right, comparison, left_supplied, right_supplied, left_projection,
                                   right_projection, ProjectionChild::dict_value, 0,
                                   &DictScalarType::mapped_type));
    }
    if (dynamic_cast<const TypeScalarType *>(&left) != nullptr &&
        dynamic_cast<const TypeScalarType *>(&right) != nullptr) {
        return compare_child<TypeScalarType>(
            left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
            ProjectionChild::scalar_type, 0, &TypeScalarType::type);
    }
    if (const auto *left_value = type_cast<TimeSeriesValueType>(&left)) {
        if (const auto *right_value = type_cast<TimeSeriesValueType>(&right)) {
            auto result = compare_child<TimeSeriesValueType>(
                left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
                ProjectionChild::time_series_value, 0,
                &TimeSeriesValueType::value_type);
            return combine(result, compare_guarantees(left_value->guarantee(), right_value->guarantee(), comparison));
        }
    }
    if (const auto *left_list = type_cast<TimeSeriesListType>(&left)) {
        if (const auto *right_list = type_cast<TimeSeriesListType>(&right)) {
            auto result = compare_child<TimeSeriesListType>(
                left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
                ProjectionChild::time_series_list_item, 0,
                [](const auto &list) -> const Type & { return list.item_type(); });
            result = combine(result, compare_sizes(left_list->size(), right_list->size(), comparison));
            return combine(result, compare_guarantees(left_list->guarantee(), right_list->guarantee(), comparison));
        }
    }
    if (const auto *left_set = type_cast<TimeSeriesSetType>(&left)) {
        if (const auto *right_set = type_cast<TimeSeriesSetType>(&right)) {
            auto result = compare_child<TimeSeriesSetType>(
                left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
                ProjectionChild::time_series_set_item, 0, &TimeSeriesSetType::item_type);
            return combine(result, compare_guarantees(left_set->guarantee(), right_set->guarantee(), comparison));
        }
    }
    if (const auto *left_dict = type_cast<TimeSeriesDictionaryType>(&left)) {
        if (const auto *right_dict = type_cast<TimeSeriesDictionaryType>(&right)) {
            auto result = compare_child<TimeSeriesDictionaryType>(
                left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
                ProjectionChild::time_series_dict_key, 0, &TimeSeriesDictionaryType::key_type);
            result = combine(result, compare_child<TimeSeriesDictionaryType>(
                                         left, right, comparison, left_supplied, right_supplied, left_projection,
                                         right_projection, ProjectionChild::time_series_dict_value, 0,
                                         &TimeSeriesDictionaryType::value_type));
            return combine(result, compare_guarantees(left_dict->guarantee(), right_dict->guarantee(), comparison));
        }
    }
    if (const auto *left_bundle = type_cast<TimeSeriesBundleType>(&left)) {
        if (const auto *right_bundle = type_cast<TimeSeriesBundleType>(&right)) {
            auto result = compare_child<TimeSeriesBundleType>(
                left, right, comparison, left_supplied, right_supplied, left_projection, right_projection,
                ProjectionChild::time_series_bundle_schema, 0, &TimeSeriesBundleType::schema_type);
            return combine(result, compare_guarantees(left_bundle->guarantee(), right_bundle->guarantee(), comparison));
        }
    }
    if (const auto *left_schema = dynamic_cast<const Schema *>(&left)) {
        if (const auto *right_schema = dynamic_cast<const Schema *>(&right)) {
            if (left_schema->named() && right_schema->named()) {
                const auto left_derives = left_schema->derives_from(*right_schema);
                const auto right_derives = right_schema->derives_from(*left_schema);
                if (left_derives && right_derives) {
                    return Order::equivalent;
                }
                if (left_derives) {
                    return Order::less;
                }
                if (right_derives) {
                    return Order::greater;
                }
            }
        }
    }

    if (&left == &right) {
        return Order::equivalent;
    }

    return compare_resolved(left, right);
}

Order repeated_variable_order(const Comparison &comparison) {
    auto result = Order::equivalent;
    for (auto left = std::size_t{}; left < comparison.declarations.size(); ++left) {
        for (auto right = left + 1; right < comparison.declarations.size(); ++right) {
            const auto left_repeated = comparison.declarations[left].first.variable != nullptr &&
                                       comparison.declarations[left].first == comparison.declarations[right].first;
            const auto right_repeated = comparison.declarations[left].second.variable != nullptr &&
                                        comparison.declarations[left].second == comparison.declarations[right].second;
            if (left_repeated == right_repeated) {
                continue;
            }
            const auto &other_left = comparison.declarations[left].second;
            const auto &other_right = comparison.declarations[right].second;
            if (left_repeated && other_left == other_right) {
                continue;
            }
            const auto &this_left = comparison.declarations[left].first;
            const auto &that_left = comparison.declarations[right].first;
            if (right_repeated && this_left == that_left) {
                continue;
            }
            result = combine(result, left_repeated ? Order::less : Order::greater);
        }
    }
    return result;
}

} // namespace

std::partial_ordering compare_specificity(const Signature &left, const Signature &right) {
    if (left.parameter_count() != right.parameter_count()) {
        return Order::unordered;
    }

    return details::compare_parameter_specificity(left, right, left.parameter_count());
}

std::partial_ordering details::compare_parameter_specificity(const Signature &left, const Signature &right,
                                                             std::size_t parameter_count) {
    return details::compare_parameter_specificity(left, right, {}, {}, parameter_count);
}

std::partial_ordering details::compare_parameter_specificity(const Signature &left, const Signature &right,
                                                             std::span<const Type *const> left_supplied_types,
                                                             std::span<const Type *const> right_supplied_types,
                                                             std::size_t parameter_count) {
    auto comparison = Comparison{};
    for (auto index = std::size_t{}; index < parameter_count; ++index) {
        const auto &left_parameter = left.parameter(index);
        const auto &right_parameter = right.parameter(index);
        const auto *left_supplied = index < left_supplied_types.size() ? left_supplied_types[index] : nullptr;
        const auto *right_supplied = index < right_supplied_types.size() ? right_supplied_types[index] : nullptr;
        comparison.order = combine(comparison.order, compare_types(*left_parameter.type, *right_parameter.type,
                                                                   comparison, left_supplied, right_supplied));
        const auto constraint_order = [&] {
            const auto *left_constraint = left_parameter.graph_constraint.get();
            const auto *right_constraint = right_parameter.graph_constraint.get();
            const auto left_constrained = left_constraint != nullptr && left_constraint->constrained();
            const auto right_constrained = right_constraint != nullptr && right_constraint->constrained();
            if (!left_constrained) {
                return right_constrained ? std::partial_ordering::greater : std::partial_ordering::equivalent;
            }
            if (!right_constrained) {
                return std::partial_ordering::less;
            }
            return left_constraint->compare_specificity(*right_constraint);
        }();
        comparison.order = combine(comparison.order, constraint_order);
    }
    return combine(comparison.order, repeated_variable_order(comparison));
}

} // namespace bg
