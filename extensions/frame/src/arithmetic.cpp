#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/library/operators.hpp>
#include <bg/util/symbol.hpp>

#include <memory>

namespace bg::frame {
namespace {

using namespace bg::literals;

template <template <typename, auto...> typename Field>
using LEFT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_ADD_LEFT_EXPRESSION", Field>;
template <template <typename, auto...> typename Field>
using RIGHT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_ADD_RIGHT_EXPRESSION", Field>;
using AdditionLhsOperand = Union<"FRAME_COLUMN_ADDITION_LHS", TSC<double>, TS<double>, TSB<LEFT_EXPRESSION>>;
using AdditionRhsOperand = Union<"FRAME_COLUMN_ADDITION_RHS", TSC<double>, TS<double>, TSB<RIGHT_EXPRESSION>>;
template <template <typename, auto...> typename Field>
using LEFT_SUBTRACTION_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_SUB_LEFT_EXPRESSION", Field>;
template <template <typename, auto...> typename Field>
using RIGHT_SUBTRACTION_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_SUB_RIGHT_EXPRESSION", Field>;
using SubtractionLhsOperand =
    Union<"FRAME_COLUMN_SUBTRACTION_LHS", TSC<double>, TS<double>, TSB<LEFT_SUBTRACTION_EXPRESSION>>;
using SubtractionRhsOperand =
    Union<"FRAME_COLUMN_SUBTRACTION_RHS", TSC<double>, TS<double>, TSB<RIGHT_SUBTRACTION_EXPRESSION>>;

bool is_column_arithmetic_operand(const Type &type) {
    const auto *time_series = type_cast<TimeSeriesType>(&type);
    if (time_series == nullptr)
        return false;
    const auto &concrete_type = time_series->dereferenced_type();
    return TSC<double>::type().accepts(concrete_type) || details::is_column_expression_bundle_type(concrete_type);
}

void add_double_operands(std::span<const row_id_t> ids, const ColumnOperandBatch<double> &lhs,
                         const ColumnOperandBatch<double> &rhs, MutableColumnBatch<double> output) {
    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        const auto valid = lhs.is_valid(row) && rhs.is_valid(row);
        output.set_valid(row, valid);
        if (valid)
            output.values[row] = lhs.value(row) + rhs.value(row);
    }
}

void subtract_double_operands(std::span<const row_id_t> ids, const ColumnOperandBatch<double> &lhs,
                              const ColumnOperandBatch<double> &rhs, MutableColumnBatch<double> output) {
    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        const auto valid = lhs.is_valid(row) && rhs.is_valid(row);
        output.set_valid(row, valid);
        if (valid)
            output.values[row] = lhs.value(row) - rhs.value(row);
    }
}

const auto registered_double_binary_adapter = [] {
    register_column_block_function<double, double, double>();
    return true;
}();

const auto addition_graph = graph(
    "name"_n = "frame_double_column_add",
    require = [](const WiringArguments &arguments) {
        return is_column_arithmetic_operand(arguments.original_type(0)) ||
               is_column_arithmetic_operand(arguments.original_type(1));
    },
    [](wp<AdditionLhsOperand> lhs, wp<AdditionRhsOperand> rhs) {
        return make_column_expression(ColumnBlockFunction<double, double, double>{&add_double_operands}, lhs, rhs);
    });

const auto registered = operators::add.overload(addition_graph);

const auto column_constant_addition_graph = graph(
    "name"_n = "frame_double_column_scalar_add",
    require = [](const WiringArguments &arguments) {
        return is_column_arithmetic_operand(arguments.original_type(0));
    },
    [](wp<AdditionLhsOperand> column, double value) -> wp<TSC<double>> {
        return operators::add(column, operators::const_(value).as<TS<double>>()).as<TSC<double>>();
    });
const auto registered_column_constant = operators::add.overload(column_constant_addition_graph);

const auto scalar_column_addition_graph = graph(
    "name"_n = "frame_double_scalar_column_add",
    require = [](const WiringArguments &arguments) {
        return is_column_arithmetic_operand(arguments.original_type(1));
    },
    [](double value, wp<AdditionRhsOperand> column) -> wp<TSC<double>> {
        return operators::add(operators::const_(value).as<TS<double>>(), column).as<TSC<double>>();
    });
const auto registered_scalar_column = operators::add.overload(scalar_column_addition_graph);

const auto subtraction_graph = graph(
    "name"_n = "frame_double_column_subtract",
    require = [](const WiringArguments &arguments) {
        return is_column_arithmetic_operand(arguments.original_type(0)) ||
               is_column_arithmetic_operand(arguments.original_type(1));
    },
    [](wp<SubtractionLhsOperand> lhs, wp<SubtractionRhsOperand> rhs) {
        return make_column_expression(ColumnBlockFunction<double, double, double>{&subtract_double_operands}, lhs,
                                      rhs);
    });
const auto registered_subtraction = operators::sub.overload(subtraction_graph);

const auto column_constant_subtraction_graph = graph(
    "name"_n = "frame_double_column_scalar_subtract",
    require = [](const WiringArguments &arguments) {
        return is_column_arithmetic_operand(arguments.original_type(0));
    },
    [](wp<SubtractionLhsOperand> column, double value) -> wp<TSC<double>> {
        return operators::sub(column, operators::const_(value).as<TS<double>>()).as<TSC<double>>();
    });
const auto registered_column_constant_subtraction = operators::sub.overload(column_constant_subtraction_graph);

const auto scalar_column_subtraction_graph = graph(
    "name"_n = "frame_double_scalar_column_subtract",
    require = [](const WiringArguments &arguments) {
        return is_column_arithmetic_operand(arguments.original_type(1));
    },
    [](double value, wp<SubtractionRhsOperand> column) -> wp<TSC<double>> {
        return operators::sub(operators::const_(value).as<TS<double>>(), column).as<TSC<double>>();
    });
const auto registered_scalar_column_subtraction = operators::sub.overload(scalar_column_subtraction_graph);

} // namespace
} // namespace bg::frame
