#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/library/operators.hpp>
#include <bg/util/symbol.hpp>

#include "operand_type.hpp"

#include <functional>
#include <string_view>

namespace bg::frame {
namespace {

using namespace bg::literals;

template <template <typename, auto...> typename Field>
using LEFT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_COMPARISON_LEFT_EXPRESSION", Field>;
template <template <typename, auto...> typename Field>
using RIGHT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_COMPARISON_RIGHT_EXPRESSION", Field>;

template <cell_type T, typename Comparator>
void compare_operands(std::span<const row_id_t> ids, const ColumnOperandBatch<T> &lhs,
                      const ColumnOperandBatch<T> &rhs, MutableColumnBatch<bool> output) {
    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        const auto valid = lhs.is_valid(row) && rhs.is_valid(row);
        output.set_valid(row, valid);
        if (valid)
            output.set_value(row, std::invoke(Comparator{}, lhs.value(row), rhs.value(row)));
    }
}

template <cell_type T, typename Comparator, typename Operator>
void register_comparison(Operator &operation, std::string_view name) {
    using LhsOperand = Union<"FRAME_COMPARISON_LHS", TSC<T>, TS<T>, TSB<LEFT_EXPRESSION>>;
    using RhsOperand = Union<"FRAME_COMPARISON_RHS", TSC<T>, TS<T>, TSB<RIGHT_EXPRESSION>>;

    const auto graph_name = std::string{"frame_"} + std::string{name};
    const auto binary_graph = graph(
        "name"_n = util::symbol{graph_name},
        require = [](const WiringArguments &arguments) {
            const auto &lhs = arguments.original_type(0);
            const auto &rhs = arguments.original_type(1);
            return details::is_operand(lhs, cpp_scalar_type<T>()) &&
                   details::is_operand(rhs, cpp_scalar_type<T>()) &&
                   (details::is_column_operand(lhs, cpp_scalar_type<T>()) ||
                    details::is_column_operand(rhs, cpp_scalar_type<T>()));
        },
        [](wp<LhsOperand> lhs, wp<RhsOperand> rhs) {
            return make_column_expression(ColumnBlockFunction<bool, T, T>{&compare_operands<T, Comparator>}, lhs,
                                          rhs);
        });
    operation.overload(binary_graph);

    const auto column_parameter_graph = graph(
        "name"_n = util::symbol{graph_name + "_column_parameter"},
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(0), cpp_scalar_type<T>());
        },
        [operation_ptr = std::addressof(operation)](wp<LhsOperand> column, T value) -> wp<TSC<bool>> {
            return (*operation_ptr)(column, operators::const_(value).template as<TS<T>>()).template as<TSC<bool>>();
        });
    operation.overload(column_parameter_graph);

    const auto parameter_column_graph = graph(
        "name"_n = util::symbol{graph_name + "_parameter_column"},
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(1), cpp_scalar_type<T>());
        },
        [operation_ptr = std::addressof(operation)](T value, wp<RhsOperand> column) -> wp<TSC<bool>> {
            return (*operation_ptr)(operators::const_(value).template as<TS<T>>(), column).template as<TSC<bool>>();
        });
    operation.overload(parameter_column_graph);
}

const auto registered = [] {
    register_column_block_function<bool, i64, i64>();
    register_column_block_function<bool, double, double>();

    register_comparison<i64, std::equal_to<>>(operators::eq, "i64_equal");
    register_comparison<i64, std::not_equal_to<>>(operators::ne, "i64_not_equal");
    register_comparison<i64, std::less<>>(operators::lt, "i64_less");
    register_comparison<i64, std::less_equal<>>(operators::le, "i64_less_equal");
    register_comparison<i64, std::greater<>>(operators::gt, "i64_greater");
    register_comparison<i64, std::greater_equal<>>(operators::ge, "i64_greater_equal");

    register_comparison<double, std::equal_to<>>(operators::eq, "double_equal");
    register_comparison<double, std::not_equal_to<>>(operators::ne, "double_not_equal");
    register_comparison<double, std::less<>>(operators::lt, "double_less");
    register_comparison<double, std::less_equal<>>(operators::le, "double_less_equal");
    register_comparison<double, std::greater<>>(operators::gt, "double_greater");
    register_comparison<double, std::greater_equal<>>(operators::ge, "double_greater_equal");
    return true;
}();

} // namespace
} // namespace bg::frame
