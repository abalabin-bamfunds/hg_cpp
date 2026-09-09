#include <bg/type/signature.hpp>

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
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace bg {
namespace {

using Order = std::partial_ordering;

struct Comparison {
    Order order{Order::equivalent};
    std::vector<std::pair<const Variable *, const Variable *>> variables;
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

Order compare_types(const Type &left, const Type &right, Comparison &comparison);

template <typename T> bool contains(std::span<const T> values, T value) {
    return std::ranges::find(values, value) != values.end();
}

template <typename T> Order compare_sets(std::span<const T> left, std::span<const T> right) {
    if (left.empty() && right.empty()) {
        return Order::equivalent;
    }
    if (left.empty()) {
        return Order::greater;
    }
    if (right.empty()) {
        return Order::less;
    }

    const auto left_subset = std::ranges::all_of(left, [&](T value) { return contains(right, value); });
    const auto right_subset = std::ranges::all_of(right, [&](T value) { return contains(left, value); });
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

template <typename Category>
bool alternatives_subset(const TypeVariableType<Category> &left, const TypeVariableType<Category> &right,
                         Comparison &comparison) {
    if (right.alternatives().empty()) {
        return true;
    }
    if (left.alternatives().empty()) {
        return false;
    }

    auto complete = comparison;
    const auto subset = std::ranges::all_of(left.alternatives(), [&](const auto *left_alternative) {
        for (const auto *right_alternative : right.alternatives()) {
            auto trial = complete;
            const auto order = compare_types(*left_alternative, *right_alternative, trial);
            if (order == Order::less || order == Order::equivalent) {
                complete = std::move(trial);
                return true;
            }
        }
        return false;
    });
    if (subset) {
        comparison = std::move(complete);
    }
    return subset;
}

template <typename Category>
Order compare_variables(const TypeVariableType<Category> &left, const TypeVariableType<Category> &right,
                        Comparison &comparison) {
    comparison.variables.emplace_back(&left, &right);

    const auto left_subset = alternatives_subset(left, right, comparison);
    const auto right_subset = alternatives_subset(right, left, comparison);
    const auto alternatives = left_subset == right_subset ? (left_subset ? Order::equivalent : Order::unordered)
                                                          : (left_subset ? Order::less : Order::greater);
    const auto constraints = compare_sets(right.constraints(), left.constraints());
    return combine(alternatives, constraints);
}

template <typename Category> bool variable_accepts(const TypeVariableType<Category> &variable, const Type &type) {
    if (type.is_resolved()) {
        return variable.accepts(type);
    }

    if constexpr (std::same_as<Category, Type>) {
        return variable.alternatives().empty();
    } else {
        const auto *category = dynamic_cast<const Category *>(&type);
        if (category == nullptr || !variable.alternatives().empty()) {
            return false;
        }
        return std::ranges::all_of(variable.constraints(),
                                   [category](auto constraint) { return constraint(*category); });
    }
}

struct DirectVariable {
    const Variable *identity;
    enum class Category { any, scalar, time_series, schema } category;
};

std::optional<DirectVariable> direct_variable(const Type &type) noexcept {
    if (const auto *variable = dynamic_cast<const TypeVariableType<Type> *>(&type)) {
        return DirectVariable{variable, DirectVariable::Category::any};
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<ScalarType> *>(&type)) {
        return DirectVariable{variable, DirectVariable::Category::scalar};
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<TimeSeriesType> *>(&type)) {
        return DirectVariable{variable, DirectVariable::Category::time_series};
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<BundleSchemaType> *>(&type)) {
        return DirectVariable{variable, DirectVariable::Category::schema};
    }
    return std::nullopt;
}

Order compare_direct_variables(const Type &left, const Type &right, Comparison &comparison) {
    if (const auto *left_variable = dynamic_cast<const TypeVariableType<Type> *>(&left)) {
        if (const auto *right_variable = dynamic_cast<const TypeVariableType<Type> *>(&right)) {
            return compare_variables(*left_variable, *right_variable, comparison);
        }
    }
    if (const auto *left_variable = dynamic_cast<const TypeVariableType<ScalarType> *>(&left)) {
        if (const auto *right_variable = dynamic_cast<const TypeVariableType<ScalarType> *>(&right)) {
            return compare_variables(*left_variable, *right_variable, comparison);
        }
    }
    if (const auto *left_variable = dynamic_cast<const TypeVariableType<TimeSeriesType> *>(&left)) {
        if (const auto *right_variable = dynamic_cast<const TypeVariableType<TimeSeriesType> *>(&right)) {
            return compare_variables(*left_variable, *right_variable, comparison);
        }
    }
    if (const auto *left_variable = dynamic_cast<const TypeVariableType<BundleSchemaType> *>(&left)) {
        if (const auto *right_variable = dynamic_cast<const TypeVariableType<BundleSchemaType> *>(&right)) {
            return compare_variables(*left_variable, *right_variable, comparison);
        }
    }

    const auto left_variable = direct_variable(left);
    const auto right_variable = direct_variable(right);
    comparison.variables.emplace_back(left_variable->identity, right_variable->identity);
    if (left_variable->category == right_variable->category) {
        return Order::unordered;
    }
    if (left_variable->category == DirectVariable::Category::any) {
        return Order::greater;
    }
    if (right_variable->category == DirectVariable::Category::any) {
        return Order::less;
    }
    return Order::unordered;
}

template <typename Category>
std::optional<Order> compare_one_variable(const Type &left, const Type &right,
                                          const TypeVariableType<Category> *left_variable,
                                          const TypeVariableType<Category> *right_variable) {
    if (left_variable != nullptr && right_variable == nullptr) {
        return variable_accepts(*left_variable, right) ? Order::greater : Order::unordered;
    }
    if (left_variable == nullptr && right_variable != nullptr) {
        return variable_accepts(*right_variable, left) ? Order::less : Order::unordered;
    }
    return std::nullopt;
}

Order compare_guarantees(const GuaranteeParameter &left, const GuaranteeParameter &right, Comparison &comparison) {
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

    comparison.variables.emplace_back(left_variable, right_variable);
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

    comparison.variables.emplace_back(left.variable(), right.variable());
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

Order compare_types(const Type &left, const Type &right, Comparison &comparison) {
    const auto *left_reference = dynamic_cast<const TimeSeriesReferenceType *>(&left);
    const auto *right_reference = dynamic_cast<const TimeSeriesReferenceType *>(&right);
    if (left_reference != nullptr || right_reference != nullptr) {
        const auto &left_target = left_reference == nullptr ? left : left_reference->target_type();
        const auto &right_target = right_reference == nullptr ? right : right_reference->target_type();
        return compare_types(left_target, right_target, comparison);
    }

    const auto left_direct_variable = direct_variable(left);
    const auto right_direct_variable = direct_variable(right);
    if (left_direct_variable && right_direct_variable) {
        return compare_direct_variables(left, right, comparison);
    }

    if (const auto result = compare_one_variable(left, right, dynamic_cast<const TypeVariableType<Type> *>(&left),
                                                 dynamic_cast<const TypeVariableType<Type> *>(&right))) {
        return *result;
    }
    if (const auto result = compare_one_variable(left, right, dynamic_cast<const TypeVariableType<ScalarType> *>(&left),
                                                 dynamic_cast<const TypeVariableType<ScalarType> *>(&right))) {
        return *result;
    }
    if (const auto result =
            compare_one_variable(left, right, dynamic_cast<const TypeVariableType<TimeSeriesType> *>(&left),
                                 dynamic_cast<const TypeVariableType<TimeSeriesType> *>(&right))) {
        return *result;
    }
    if (const auto result =
            compare_one_variable(left, right, dynamic_cast<const TypeVariableType<BundleSchemaType> *>(&left),
                                 dynamic_cast<const TypeVariableType<BundleSchemaType> *>(&right))) {
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
        if (!variable_accepts(variable, other)) {
            return Order::unordered;
        }
        return left_series_variable == nullptr ? Order::less : Order::greater;
    }

    if (&left == &right) {
        return Order::equivalent;
    }

    if (const auto *left_list = dynamic_cast<const ListScalarType *>(&left)) {
        if (const auto *right_list = dynamic_cast<const ListScalarType *>(&right)) {
            return compare_types(left_list->item_type(), right_list->item_type(), comparison);
        }
    }
    if (const auto *left_set = dynamic_cast<const SetScalarType *>(&left)) {
        if (const auto *right_set = dynamic_cast<const SetScalarType *>(&right)) {
            return compare_types(left_set->item_type(), right_set->item_type(), comparison);
        }
    }
    if (const auto *left_dict = dynamic_cast<const DictScalarType *>(&left)) {
        if (const auto *right_dict = dynamic_cast<const DictScalarType *>(&right)) {
            auto result = compare_types(left_dict->key_type(), right_dict->key_type(), comparison);
            return combine(result, compare_types(left_dict->mapped_type(), right_dict->mapped_type(), comparison));
        }
    }
    if (const auto *left_type = dynamic_cast<const TypeScalarType *>(&left)) {
        if (const auto *right_type = dynamic_cast<const TypeScalarType *>(&right)) {
            return compare_types(left_type->type(), right_type->type(), comparison);
        }
    }
    if (const auto *left_value = dynamic_cast<const TimeSeriesValueType *>(&left)) {
        if (const auto *right_value = dynamic_cast<const TimeSeriesValueType *>(&right)) {
            auto result = compare_types(left_value->value_type(), right_value->value_type(), comparison);
            return combine(result, compare_guarantees(left_value->guarantee(), right_value->guarantee(), comparison));
        }
    }
    if (const auto *left_list = dynamic_cast<const TimeSeriesListType *>(&left)) {
        if (const auto *right_list = dynamic_cast<const TimeSeriesListType *>(&right)) {
            auto result = compare_types(left_list->item_type(), right_list->item_type(), comparison);
            result = combine(result, compare_sizes(left_list->size(), right_list->size(), comparison));
            return combine(result, compare_guarantees(left_list->guarantee(), right_list->guarantee(), comparison));
        }
    }
    if (const auto *left_set = dynamic_cast<const TimeSeriesSetType *>(&left)) {
        if (const auto *right_set = dynamic_cast<const TimeSeriesSetType *>(&right)) {
            auto result = compare_types(left_set->item_type(), right_set->item_type(), comparison);
            return combine(result, compare_guarantees(left_set->guarantee(), right_set->guarantee(), comparison));
        }
    }
    if (const auto *left_dict = dynamic_cast<const TimeSeriesDictionaryType *>(&left)) {
        if (const auto *right_dict = dynamic_cast<const TimeSeriesDictionaryType *>(&right)) {
            auto result = compare_types(left_dict->key_type(), right_dict->key_type(), comparison);
            result = combine(result, compare_types(left_dict->value_type(), right_dict->value_type(), comparison));
            return combine(result, compare_guarantees(left_dict->guarantee(), right_dict->guarantee(), comparison));
        }
    }
    if (const auto *left_bundle = dynamic_cast<const TimeSeriesBundleType *>(&left)) {
        if (const auto *right_bundle = dynamic_cast<const TimeSeriesBundleType *>(&right)) {
            auto result = compare_types(left_bundle->schema_type(), right_bundle->schema_type(), comparison);
            return combine(result, compare_guarantees(left_bundle->guarantee(), right_bundle->guarantee(), comparison));
        }
    }
    if (const auto *left_schema = dynamic_cast<const BundleSchema *>(&left)) {
        if (const auto *right_schema = dynamic_cast<const BundleSchema *>(&right)) {
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

    return compare_resolved(left, right);
}

Order repeated_variable_order(const Comparison &comparison) {
    auto result = Order::equivalent;
    for (auto left = std::size_t{}; left < comparison.variables.size(); ++left) {
        for (auto right = left + 1; right < comparison.variables.size(); ++right) {
            const auto left_repeated = comparison.variables[left].first == comparison.variables[right].first;
            const auto right_repeated = comparison.variables[left].second == comparison.variables[right].second;
            if (left_repeated == right_repeated) {
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

    auto comparison = Comparison{};
    for (auto index = std::size_t{}; index < left.parameter_count(); ++index) {
        comparison.order = combine(
            comparison.order, compare_types(*left.parameter(index).type, *right.parameter(index).type, comparison));
    }
    return combine(comparison.order, repeated_variable_order(comparison));
}

} // namespace bg
