#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/library/operators.hpp>
#include <bg/util/symbol.hpp>

#include "operand_type.hpp"
#include "mixed_expression.hpp"

#include <memory>

namespace bg::frame {
namespace {

using namespace bg::literals;

template <template <typename, auto...> typename Field>
using LEFT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_LOGICAL_LEFT_EXPRESSION", Field>;
template <template <typename, auto...> typename Field>
using RIGHT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_LOGICAL_RIGHT_EXPRESSION", Field>;

using LogicalLhsOperand = Union<"FRAME_LOGICAL_LHS", TSC<bool>, TS<bool>, TSB<LEFT_EXPRESSION>>;
using LogicalRhsOperand = Union<"FRAME_LOGICAL_RHS", TSC<bool>, TS<bool>, TSB<RIGHT_EXPRESSION>>;

void logical_and(std::span<const row_id_t> rows, const ColumnOperandBatch<bool> &lhs,
                 const ColumnOperandBatch<bool> &rhs, MutableColumnBatch<bool> output) {
    for (auto position = std::size_t{}; position != rows.size(); ++position) {
        const auto lhs_valid = lhs.is_valid(position);
        const auto rhs_valid = rhs.is_valid(position);
        const auto known_false = (lhs_valid && !lhs.value(position)) || (rhs_valid && !rhs.value(position));
        if (known_false) {
            output.set_value(position, false);
            output.set_valid(position, true);
            continue;
        }
        if (lhs_valid && rhs_valid) {
            output.set_value(position, true);
            output.set_valid(position, true);
            continue;
        }
        output.set_valid(position, false);
    }
}

void logical_or(std::span<const row_id_t> rows, const ColumnOperandBatch<bool> &lhs,
                const ColumnOperandBatch<bool> &rhs, MutableColumnBatch<bool> output) {
    for (auto position = std::size_t{}; position != rows.size(); ++position) {
        const auto lhs_valid = lhs.is_valid(position);
        const auto rhs_valid = rhs.is_valid(position);
        const auto known_true = (lhs_valid && lhs.value(position)) || (rhs_valid && rhs.value(position));
        if (known_true) {
            output.set_value(position, true);
            output.set_valid(position, true);
            continue;
        }
        if (lhs_valid && rhs_valid) {
            output.set_value(position, false);
            output.set_valid(position, true);
            continue;
        }
        output.set_valid(position, false);
    }
}

void logical_not(std::span<const row_id_t> rows, const ColumnOperandBatch<bool> &value,
                 MutableColumnBatch<bool> output) {
    for (auto position = std::size_t{}; position != rows.size(); ++position) {
        const auto valid = value.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.set_value(position, !value.value(position));
    }
}

template <typename Operator>
void register_binary_logical_operator(Operator &operation, std::string_view name,
                                      ColumnBlockFunction<bool, bool, bool>::function_type function) {
    const auto graph_name = std::string{"frame_"} + std::string{name};
    const auto binary_graph = graph(
        "name"_n = util::symbol{graph_name},
        require = [](const WiringArguments &arguments) {
            const auto &lhs = arguments.original_type(0);
            const auto &rhs = arguments.original_type(1);
            return details::is_operand(lhs, cpp_scalar_type<bool>()) &&
                   details::is_operand(rhs, cpp_scalar_type<bool>()) &&
                   (details::is_column_operand(lhs, cpp_scalar_type<bool>()) ||
                    details::is_column_operand(rhs, cpp_scalar_type<bool>()));
        },
        [function](wp<LogicalLhsOperand> lhs, wp<LogicalRhsOperand> rhs) {
            return make_column_expression(ColumnBlockFunction<bool, bool, bool>{function}, lhs, rhs);
        });
    operation.overload(binary_graph);

    auto *operation_ptr = std::addressof(operation);
    const auto column_parameter_graph = graph(
        "name"_n = util::symbol{graph_name + "_column_parameter"},
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(0), cpp_scalar_type<bool>());
        },
        [operation_ptr](wp<LogicalLhsOperand> column, bool value) -> wp<TSC<bool>> {
            return (*operation_ptr)(column, operators::const_(value).template as<TS<bool>>()).template as<TSC<bool>>();
        });
    operation.overload(column_parameter_graph);

    const auto parameter_column_graph = graph(
        "name"_n = util::symbol{graph_name + "_parameter_column"},
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(1), cpp_scalar_type<bool>());
        },
        [operation_ptr](bool value, wp<LogicalRhsOperand> column) -> wp<TSC<bool>> {
            return (*operation_ptr)(operators::const_(value).template as<TS<bool>>(), column).template as<TSC<bool>>();
        });
    operation.overload(parameter_column_graph);
}

const auto registered = [] {
    register_column_block_function<bool, bool, bool>();
    register_column_block_function<bool, bool>();
    register_binary_logical_operator(operators::and_, "boolean_and", &logical_and);
    register_binary_logical_operator(operators::or_, "boolean_or", &logical_or);

    const auto not_graph = graph(
        "name"_n = "frame_boolean_not",
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(0), cpp_scalar_type<bool>());
        },
        [](wp<LogicalLhsOperand> value) {
            return make_column_expression(ColumnBlockFunction<bool, bool>{&logical_not}, value);
        });
    operators::not_.overload(not_graph);

    const auto boolean_operand = [](const Type &type) { return details::is_operand(type, cpp_scalar_type<bool>()); };
    const auto boolean_scalar = [](const Type &type) {
        return std::addressof(type) == std::addressof(cpp_scalar_type<bool>());
    };
    details::register_mixed_binary(operators::and_, details::ExpressionOperation::and_, "and", boolean_operand,
                                   boolean_scalar);
    details::register_mixed_binary(operators::or_, details::ExpressionOperation::or_, "or", boolean_operand,
                                   boolean_scalar);
    details::register_mixed_unary(operators::not_, details::ExpressionOperation::not_, "not");
    return true;
}();

} // namespace
} // namespace bg::frame
