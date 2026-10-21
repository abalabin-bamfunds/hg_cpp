#pragma once

#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/overload.hpp>
#include <bg/schema/reflection.hpp>

#include <array>

namespace bg::frame::details {

using MIXED_NUMERIC_SCALAR = ScalarVariable<"FRAME_MIXED_NUMERIC_SCALAR", i64, double, Expression>;
using MIXED_BOOLEAN_SCALAR = ScalarVariable<"FRAME_MIXED_BOOLEAN_SCALAR", bool, Expression>;

template <template <typename, auto...> typename Field> struct MixedOneBindingSchema {
    Field<TS<Expression, TimeSeriesGuarantee::const_>> expression;
    Field<TimeSeriesVariable<"FRAME_MIXED_LEFT">> binding_0;
};

template <template <typename, auto...> typename Field>
struct MixedTwoBindingSchema : schema_extends<MixedOneBindingSchema, Field> {
    Field<TimeSeriesVariable<"FRAME_MIXED_RIGHT">> binding_1;
};

template <template <typename, auto...> typename Field> struct MixedInputLeftOneBindingSchema {
    Field<TS<Expression, TimeSeriesGuarantee::const_>> expression;
    Field<TimeSeriesVariable<"FRAME_MIXED_PATTERN_LEFT_INPUT">> binding_0;
};

template <template <typename, auto...> typename Field>
struct MixedInputLeftTwoBindingSchema : schema_extends<MixedInputLeftOneBindingSchema, Field> {
    Field<TimeSeriesVariable<"FRAME_MIXED_PATTERN_LEFT_RIGHT">> binding_1;
};

template <template <typename, auto...> typename Field> struct MixedInputRightOneBindingSchema {
    Field<TS<Expression, TimeSeriesGuarantee::const_>> expression;
    Field<TimeSeriesVariable<"FRAME_MIXED_PATTERN_RIGHT_INPUT">> binding_0;
};

template <template <typename, auto...> typename Field>
struct MixedInputRightTwoBindingSchema : schema_extends<MixedInputRightOneBindingSchema, Field> {
    Field<TimeSeriesVariable<"FRAME_MIXED_PATTERN_RIGHT_RIGHT">> binding_1;
};

using MIXED_SERIES_LEFT = Union<"FRAME_MIXED_SERIES_LEFT", TSB<MixedInputLeftOneBindingSchema>,
                                TSB<MixedInputLeftTwoBindingSchema>>;
using MIXED_SERIES_RIGHT = Union<"FRAME_MIXED_SERIES_RIGHT", TSB<MixedInputRightOneBindingSchema>,
                                 TSB<MixedInputRightTwoBindingSchema>>;
using MIXED_NUMERIC_SERIES = Union<"FRAME_MIXED_NUMERIC_SERIES", TSC<operators::NUMERIC>, TS<operators::NUMERIC>>;
using MIXED_BOOLEAN_SERIES = Union<"FRAME_MIXED_BOOLEAN_SERIES", TSC<operators::BOOL>, TS<operators::BOOL>>;

template <ExpressionOperation Operation, typename ScalarDeclaration, typename Series> struct ScalarSeriesGraph {
    Scalar<ScalarDeclaration> scalar;
    wp<Series> series;

    auto wire() const {
        const auto operands = std::array{WiringArgument{scalar}, WiringArgument{series}};
        return wp<TSB<MixedOneBindingSchema>>{make_mixed_expression(Operation, operands)};
    }

    static const Type &resolve(TimeSeriesVariable<"FRAME_MIXED_LEFT">, const WiringArguments &arguments) {
        return arguments.original_type(1);
    }
};

template <ExpressionOperation Operation, typename Series, typename ScalarDeclaration> struct SeriesScalarGraph {
    wp<Series> series;
    Scalar<ScalarDeclaration> scalar;

    auto wire() const {
        const auto operands = std::array{WiringArgument{series}, WiringArgument{scalar}};
        return wp<TSB<MixedOneBindingSchema>>{make_mixed_expression(Operation, operands)};
    }

    static const Type &resolve(TimeSeriesVariable<"FRAME_MIXED_LEFT">, const WiringArguments &arguments) {
        return arguments.original_type(0);
    }
};

template <ExpressionOperation Operation, typename Lhs, typename Rhs> struct MixedSeriesSeriesGraph {
    wp<Lhs> lhs;
    wp<Rhs> rhs;

    auto wire() const {
        const auto operands = std::array{WiringArgument{lhs}, WiringArgument{rhs}};
        return wp<TSB<MixedTwoBindingSchema>>{make_mixed_expression(Operation, operands)};
    }

    static const Type &resolve(TimeSeriesVariable<"FRAME_MIXED_LEFT">, const WiringArguments &arguments) {
        return arguments.original_type(0);
    }

    static const Type &resolve(TimeSeriesVariable<"FRAME_MIXED_RIGHT">, const WiringArguments &arguments) {
        return arguments.original_type(1);
    }
};

template <ExpressionOperation Operation> struct MixedUnaryGraph {
    wp<MIXED_SERIES_LEFT> value;

    auto wire() const {
        const auto operands = std::array{WiringArgument{value}};
        return wp<TSB<MixedOneBindingSchema>>{make_mixed_expression(Operation, operands)};
    }

    static const Type &resolve(TimeSeriesVariable<"FRAME_MIXED_LEFT">, const WiringArguments &arguments) {
        return arguments.original_type(0);
    }
};

template <auto &Operator, ExpressionOperation Operation, typename ScalarDeclaration, typename Series>
struct MixedBinaryOverloads {
    overload<Operator> expression_series{graph<ScalarSeriesGraph<Operation, Expression, Series>>()};
    overload<Operator> scalar_mixed_series{
        graph<ScalarSeriesGraph<Operation, ScalarDeclaration, MIXED_SERIES_RIGHT>>()};
    overload<Operator> series_expression{graph<SeriesScalarGraph<Operation, Series, Expression>>()};
    overload<Operator> mixed_series_scalar{
        graph<SeriesScalarGraph<Operation, MIXED_SERIES_LEFT, ScalarDeclaration>>()};
    overload<Operator> mixed_series_series{graph<MixedSeriesSeriesGraph<Operation, MIXED_SERIES_LEFT, Series>>()};
    overload<Operator> series_mixed_series{graph<MixedSeriesSeriesGraph<Operation, Series, MIXED_SERIES_RIGHT>>()};
    overload<Operator> nested_mixed_series{
        graph<MixedSeriesSeriesGraph<Operation, MIXED_SERIES_LEFT, MIXED_SERIES_RIGHT>>()};
};

template <auto &Operator, ExpressionOperation Operation> struct MixedUnaryOverloads {
    overload<Operator> not_impl{graph<MixedUnaryGraph<Operation>>()};
};

} // namespace bg::frame::details
