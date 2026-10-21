#pragma once

#include <bg/frame/expression.hpp>
#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/util/symbol.hpp>

#include <array>
#include <string>
#include <string_view>

namespace bg::frame::details {

using namespace bg::literals;

using MIXED_LEFT = TimeSeriesVariable<"FRAME_MIXED_LEFT">;
using MIXED_RIGHT = TimeSeriesVariable<"FRAME_MIXED_RIGHT">;
using MIXED_INPUT = TimeSeriesVariable<"FRAME_MIXED_INPUT">;

template <template <typename, auto...> typename Field> struct MixedOneBindingSchema {
    Field<TS<Expression, TimeSeriesGuarantee::const_>> expression;
    Field<MIXED_INPUT> binding_0;
};

template <template <typename, auto...> typename Field> struct MixedTwoBindingSchema {
    Field<TS<Expression, TimeSeriesGuarantee::const_>> expression;
    Field<MIXED_LEFT> binding_0;
    Field<MIXED_RIGHT> binding_1;
};

inline bool is_expression_type(const Type &type) {
    return std::addressof(type) == std::addressof(scalar_type_traits<Expression>::type());
}

template <typename Operator, typename OperandPredicate, typename ScalarPredicate>
void register_mixed_binary(Operator &operation, ExpressionOperation expression_operation,
                           std::string_view name, OperandPredicate is_operand,
                           ScalarPredicate is_scalar_operand) {
    const auto prefix = std::string{"frame_mixed_"} + std::string{name};
    const auto make_one_binding_result = [expression_operation](auto lhs, auto rhs) {
        const auto operands = std::array{WiringArgument{lhs}, WiringArgument{rhs}};
        return wp<TSB<MixedOneBindingSchema>>{make_mixed_expression(expression_operation, operands)};
    };
    const auto make_two_binding_result = [expression_operation](auto lhs, auto rhs) {
        const auto operands = std::array{WiringArgument{lhs}, WiringArgument{rhs}};
        return wp<TSB<MixedTwoBindingSchema>>{make_mixed_expression(expression_operation, operands)};
    };

    const auto scalar_series_graph = graph(
        "name"_n = util::symbol{prefix + "_scalar_series"},
        require = [is_operand, is_scalar_operand](const WiringArguments &arguments) {
            const auto &lhs = arguments.original_type(0);
            const auto &rhs = arguments.original_type(1);
            return (is_expression_type(lhs) &&
                    (is_operand(rhs) || is_mixed_expression_bundle_type(rhs))) ||
                   (is_mixed_expression_bundle_type(rhs) && is_scalar_operand(lhs));
        },
        [make_one_binding_result](Scalar<SCALAR> lhs, wp<MIXED_INPUT> rhs) {
            return make_one_binding_result(lhs, rhs);
        });
    operation.overload(scalar_series_graph);

    const auto series_scalar_graph = graph(
        "name"_n = util::symbol{prefix + "_series_scalar"},
        require = [is_operand, is_scalar_operand](const WiringArguments &arguments) {
            const auto &lhs = arguments.original_type(0);
            const auto &rhs = arguments.original_type(1);
            return (is_operand(lhs) && is_expression_type(rhs)) ||
                   (is_mixed_expression_bundle_type(lhs) && is_expression_type(rhs)) ||
                   (is_mixed_expression_bundle_type(lhs) && is_scalar_operand(rhs));
        },
        [make_one_binding_result](wp<MIXED_INPUT> lhs, Scalar<SCALAR> rhs) {
            return make_one_binding_result(lhs, rhs);
        });
    operation.overload(series_scalar_graph);

    const auto series_series_graph = graph(
        "name"_n = util::symbol{prefix + "_series_series"},
        require = [is_operand](const WiringArguments &arguments) {
            const auto &lhs = arguments.original_type(0);
            const auto &rhs = arguments.original_type(1);
            return (is_mixed_expression_bundle_type(lhs) &&
                    (is_operand(rhs) || is_mixed_expression_bundle_type(rhs))) ||
                   (is_operand(lhs) && is_mixed_expression_bundle_type(rhs));
        },
        [make_two_binding_result](wp<MIXED_LEFT> lhs, wp<MIXED_RIGHT> rhs) {
            return make_two_binding_result(lhs, rhs);
        });
    operation.overload(series_series_graph);
}

template <typename Operator>
void register_mixed_unary(Operator &operation, ExpressionOperation expression_operation, std::string_view name) {
    const auto mixed_graph = graph(
        "name"_n = util::symbol{std::string{"frame_mixed_"} + std::string{name}},
        require = [](const WiringArguments &arguments) {
            return is_mixed_expression_bundle_type(arguments.original_type(0));
        },
        [expression_operation](wp<MIXED_INPUT> value) {
            const auto operands = std::array{WiringArgument{value}};
            return wp<TSB<MixedOneBindingSchema>>{make_mixed_expression(expression_operation, operands)};
        });
    operation.overload(mixed_graph);
}

} // namespace bg::frame::details
