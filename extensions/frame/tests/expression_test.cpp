#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

using namespace bg;
using namespace bg::frame;

void add_batch(const AlignedColumnBatch<i64, i64> &input, MutableColumnBatch<i64> output) {
    for (auto position = std::size_t{}; position != input.ids.size(); ++position) {
        const auto valid = input.left.is_valid(position) && input.right.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.values[position] = input.left.values[position] + input.right.values[position];
    }
}

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

void test_column_block_function_provider() {
    using Function = ColumnBlockFunction<i64>;

    auto function_value = ScalarValue<Function>{Function{&add_batch}};
    const auto function = function_value.view().value();

    const std::array ids{row_id_t{7}, row_id_t{12}};
    const std::array left_values{i64{2}, i64{10}};
    const std::array right_values{i64{4}, i64{3}};
    const auto left = ColumnBatch<i64>{0, left_values.size(), left_values.data(), ColumnBatch<i64>::Validity::all_valid()};
    const auto right = ColumnBatch<i64>{0, right_values.size(), right_values.data(), ColumnBatch<i64>::Validity::all_valid()};
    const auto input = AlignedColumnBatch<i64, i64>{ids, left, right};
    std::array<i64, 2> output_values{};
    std::array<std::uint8_t, 1> output_validity{};

    function(input, MutableColumnBatch<i64>{output_values, output_validity});

    require(function.function == &add_batch && output_values == std::array<i64, 2>{6, 13} &&
                output_validity[0] == 0b11,
            "column block function scalar did not round-trip through BG storage");
    require(std::addressof(function_value.type()) ==
                std::addressof(scalar_type_traits<Function>::type()) &&
                std::addressof(function_value.runtime_type()) ==
                    std::addressof(cpp_scalar_runtime_type<Function>()),
            "column block function provider did not return canonical type and runtime objects");
}

void test_binary_expression_bundle_schema() {
    using Expression = BinaryColumnExpression<TSC<i64>, TS<i64, TimeSeriesGuarantee::const_>, i64>;
    using Type = TSB<Expression::template schema>;

    const auto &type = Type::type();
    require(type.item_count() == 3 && type.item_name(0).text() == "lhs" &&
                type.item_name(1).text() == "rhs" && type.item_name(2).text() == "compute",
            "binary expression bundle did not expose immediate operands and block function");
    require(std::addressof(type.item_type(0)) == std::addressof(TSC<i64>::type()) &&
                std::addressof(type.item_type(1)) == std::addressof(TS<i64, TimeSeriesGuarantee::const_>::type()) &&
                std::addressof(type.item_type(2)) ==
                    std::addressof(TS<ColumnBlockFunction<i64>, TimeSeriesGuarantee::const_>::type()),
            "binary expression bundle did not retain its declared operand and compute types");

    static_cast<void>(time_series_runtime_traits<Type>::get(type));
}

} // namespace

int main() {
    try {
        test_column_block_function_provider();
        test_binary_expression_bundle_schema();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
