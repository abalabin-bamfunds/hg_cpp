#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/library/operators.hpp>
#include <bg/util/symbol.hpp>

#include "operand_type.hpp"
#include "mixed_expression.hpp"

#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bg::frame {
namespace {

using namespace bg::literals;

template <template <typename, auto...> typename Field>
using LEFT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_ARITHMETIC_LEFT_EXPRESSION", Field>;
template <template <typename, auto...> typename Field>
using RIGHT_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_ARITHMETIC_RIGHT_EXPRESSION", Field>;
template <template <typename, auto...> typename Field>
using CONVERSION_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_COLUMN_CONVERSION_EXPRESSION", Field>;

template <cell_type T>
using ArithmeticLhsOperand = Union<"FRAME_ARITHMETIC_LHS", TSC<T>, TS<T>, TSB<LEFT_EXPRESSION>>;
template <cell_type T>
using ArithmeticRhsOperand = Union<"FRAME_ARITHMETIC_RHS", TSC<T>, TS<T>, TSB<RIGHT_EXPRESSION>>;
template <cell_type T>
using ConversionOperand = Union<"FRAME_CONVERSION_SOURCE", TSC<T>, TSB<CONVERSION_EXPRESSION>>;

template <cell_type T> constexpr std::string_view cell_name() {
    if constexpr (std::same_as<T, i64>)
        return "i64";
    return "double";
}

template <cell_type T> T add_values(T lhs, T rhs) {
    if constexpr (std::same_as<T, i64>) {
        return std::bit_cast<i64>(static_cast<std::uint64_t>(lhs) + static_cast<std::uint64_t>(rhs));
    } else {
        return lhs + rhs;
    }
}

template <cell_type T> T subtract_values(T lhs, T rhs) {
    if constexpr (std::same_as<T, i64>) {
        return std::bit_cast<i64>(static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs));
    } else {
        return lhs - rhs;
    }
}

template <cell_type T> T multiply_values(T lhs, T rhs) {
    if constexpr (std::same_as<T, i64>) {
        return std::bit_cast<i64>(static_cast<std::uint64_t>(lhs) * static_cast<std::uint64_t>(rhs));
    } else {
        return lhs * rhs;
    }
}

template <cell_type T> double divide_values(T lhs, T rhs) {
    if (rhs == T{})
        throw std::domain_error{"division by zero"};
    return static_cast<double>(lhs) / static_cast<double>(rhs);
}

template <cell_type SOURCE, cell_type RESULT, RESULT (*FUNCTION)(SOURCE, SOURCE)>
void binary_operands(std::span<const row_id_t> rows, const ColumnOperandBatch<SOURCE> &lhs,
                     const ColumnOperandBatch<SOURCE> &rhs, MutableColumnBatch<RESULT> output) {
    for (auto position = std::size_t{}; position != rows.size(); ++position) {
        const auto valid = lhs.is_valid(position) && rhs.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.set_value(position, FUNCTION(lhs.value(position), rhs.value(position)));
    }
}

template <cell_type T, cell_type RESULT, auto FUNCTION, typename Operator>
void register_binary_arithmetic(Operator &operation, std::string_view name) {
    using LhsOperand = ArithmeticLhsOperand<T>;
    using RhsOperand = ArithmeticRhsOperand<T>;
    using Function = ColumnBlockFunction<RESULT, T, T>;

    const auto graph_name = std::string{"frame_"} + std::string{name} + "_" + std::string{cell_name<T>()};
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
            return make_column_expression(Function{&binary_operands<T, RESULT, FUNCTION>}, lhs, rhs);
        });
    operation.overload(binary_graph);

    const auto column_parameter_graph = graph(
        "name"_n = util::symbol{graph_name + "_column_parameter"},
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(0), cpp_scalar_type<T>());
        },
        [operation_ptr = std::addressof(operation)](wp<LhsOperand> column, T value) -> wp<TSC<RESULT>> {
            return (*operation_ptr)(column, operators::const_(value).template as<TS<T>>()).template as<TSC<RESULT>>();
        });
    operation.overload(column_parameter_graph);

    const auto parameter_column_graph = graph(
        "name"_n = util::symbol{graph_name + "_parameter_column"},
        require = [](const WiringArguments &arguments) {
            return details::is_column_operand(arguments.original_type(1), cpp_scalar_type<T>());
        },
        [operation_ptr = std::addressof(operation)](T value, wp<RhsOperand> column) -> wp<TSC<RESULT>> {
            return (*operation_ptr)(operators::const_(value).template as<TS<T>>(), column).template as<TSC<RESULT>>();
        });
    operation.overload(parameter_column_graph);
}

template <cell_type TARGET, cell_type SOURCE> TARGET convert_value(SOURCE value) {
    if constexpr (std::same_as<SOURCE, i64> && std::same_as<TARGET, double>) {
        return static_cast<double>(value);
    } else {
        static_assert(std::same_as<SOURCE, double> && std::same_as<TARGET, i64>);
        const auto limit = std::ldexp(1.0, 63);
        if (std::isnan(value))
            throw std::domain_error{"cannot convert NaN to i64"};
        if (value < -limit || value >= limit)
            throw std::overflow_error{"value is outside the i64 range"};
        return static_cast<i64>(value);
    }
}

template <cell_type TARGET, cell_type SOURCE>
void convert_operands(std::span<const row_id_t> rows, const ColumnOperandBatch<SOURCE> &input,
                      MutableColumnBatch<TARGET> output) {
    for (auto position = std::size_t{}; position != rows.size(); ++position) {
        const auto valid = input.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.set_value(position, convert_value<TARGET>(input.value(position)));
    }
}

template <cell_type TARGET>
bool requests_target(const WiringArguments &arguments) {
    const auto *mapping = arguments.type_map().find(::bg::details::type_parameter<operators::CONVERT_RESULT>());
    const auto *mapped_type = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
    return mapped_type != nullptr && *mapped_type != nullptr && TSC<TARGET>::type().accepts(**mapped_type);
}

template <cell_type TARGET> void register_identity_column_conversion() {
    using Input = ConversionOperand<TARGET>;
    const auto graph_name = std::string{"frame_convert_"} + std::string{cell_name<TARGET>()};
    const auto identity_graph = graph<operators::CONVERT_RESULT>(
        "name"_n = util::symbol{graph_name},
        require = [](const WiringArguments &arguments) {
            return requests_target<TARGET>(arguments) &&
                   details::is_column_operand(arguments.original_type(0), cpp_scalar_type<TARGET>());
        },
        [](wp<Input> value) -> wp<operators::CONVERT_RESULT> {
            return wp<operators::CONVERT_RESULT>{wp<TSC<TARGET>>{value}};
        });
    operators::convert.overload(identity_graph);
}

template <cell_type TARGET, cell_type SOURCE> void register_column_conversion() {
    using Function = ColumnBlockFunction<TARGET, SOURCE>;
    const auto graph_name = std::string{"frame_convert_"} + std::string{cell_name<SOURCE>()} + "_to_" +
                            std::string{cell_name<TARGET>()};
    const auto conversion_graph = graph<operators::CONVERT_RESULT>(
        "name"_n = util::symbol{graph_name},
        require = [](const WiringArguments &arguments) {
            return requests_target<TARGET>(arguments) &&
                   details::is_column_operand(arguments.original_type(0), cpp_scalar_type<SOURCE>());
        },
        [](wp<ConversionOperand<SOURCE>> value) -> wp<operators::CONVERT_RESULT> {
            const auto result = make_column_expression(Function{&convert_operands<TARGET, SOURCE>}, value);
            return wp<operators::CONVERT_RESULT>{result};
        });
    operators::convert.overload(conversion_graph);
}

const auto registered = [] {
    register_column_block_function<double, i64>();
    register_column_block_function<i64, double>();
    register_column_block_function<i64, i64, i64>();
    register_column_block_function<double, double, double>();
    register_column_block_function<double, i64, i64>();

    register_binary_arithmetic<i64, i64, &add_values<i64>>(operators::add, "add");
    register_binary_arithmetic<double, double, &add_values<double>>(operators::add, "add");
    register_binary_arithmetic<i64, i64, &subtract_values<i64>>(operators::sub, "subtract");
    register_binary_arithmetic<double, double, &subtract_values<double>>(operators::sub, "subtract");
    register_binary_arithmetic<i64, i64, &multiply_values<i64>>(operators::mul, "multiply");
    register_binary_arithmetic<double, double, &multiply_values<double>>(operators::mul, "multiply");
    register_binary_arithmetic<i64, double, &divide_values<i64>>(operators::div, "divide");
    register_binary_arithmetic<double, double, &divide_values<double>>(operators::div, "divide");

    register_identity_column_conversion<i64>();
    register_identity_column_conversion<double>();
    register_column_conversion<double, i64>();
    register_column_conversion<i64, double>();

    const auto numeric_operand = [](const Type &type) {
        return details::is_operand(type, cpp_scalar_type<i64>()) ||
               details::is_operand(type, cpp_scalar_type<double>());
    };
    const auto numeric_scalar = [](const Type &type) {
        return std::addressof(type) == std::addressof(cpp_scalar_type<i64>()) ||
               std::addressof(type) == std::addressof(cpp_scalar_type<double>());
    };
    details::register_mixed_binary(operators::add, details::ExpressionOperation::add, "add", numeric_operand,
                                   numeric_scalar);
    details::register_mixed_binary(operators::sub, details::ExpressionOperation::sub, "subtract", numeric_operand,
                                   numeric_scalar);
    details::register_mixed_binary(operators::mul, details::ExpressionOperation::mul, "multiply", numeric_operand,
                                   numeric_scalar);
    details::register_mixed_binary(operators::div, details::ExpressionOperation::div, "divide", numeric_operand,
                                   numeric_scalar);
    return true;
}();

} // namespace
} // namespace bg::frame
