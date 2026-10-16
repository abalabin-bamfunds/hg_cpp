#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/library.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/named_argument.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct QuoteFrame {
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field>
using ProbeExpression = BundleSchemaVariableDeclaration<"FRAME_PROBE_EXPRESSION", Field>;
using ProbeForm = Union<"FRAME_PROBE_FORM", TSC<double>, TS<double>, TSB<ProbeExpression>>;

void add_batch(std::span<const row_id_t> ids, const ColumnOperandBatch<i64> &lhs,
               const ColumnOperandBatch<i64> &rhs, MutableColumnBatch<i64> output) {
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        const auto valid = lhs.is_valid(position) && rhs.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.values[position] = lhs.value(position) + rhs.value(position);
    }
}

void negate_batch(std::span<const row_id_t> ids, const ColumnOperandBatch<double> &input,
                  MutableColumnBatch<i64> output) {
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        const auto valid = input.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.values[position] = static_cast<i64>(-input.value(position));
    }
}

void add_offset_batch(std::span<const row_id_t> ids, const ColumnOperandBatch<i64> &column,
                      const ColumnOperandBatch<double> &offset, MutableColumnBatch<double> output) {
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        const auto valid = column.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.values[position] = static_cast<double>(column.value(position)) + offset.value(position);
    }
}

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

void test_column_block_function_provider() {
    using Function = ColumnBlockFunction<i64, i64, i64>;

    auto function_value = ScalarValue<Function>{Function{&add_batch}};
    const auto function = function_value.view().value();

    const std::array ids{row_id_t{7}, row_id_t{12}};
    const std::array left_values{i64{2}, i64{10}};
    const std::array right_values{i64{4}, i64{3}};
    const auto left = ColumnBatch<i64>{0, left_values.size(), left_values.data(), ColumnBatch<i64>::Validity::all_valid()};
    const auto right = ColumnBatch<i64>{0, right_values.size(), right_values.data(), ColumnBatch<i64>::Validity::all_valid()};
    std::array<i64, 2> output_values{};
    std::array<std::uint8_t, 1> output_validity{};

    function(ids, ColumnOperandBatch<i64>{left}, ColumnOperandBatch<i64>{right},
             MutableColumnBatch<i64>{output_values, output_validity});

    require(function.function == &add_batch && output_values == std::array<i64, 2>{6, 13} &&
                output_validity[0] == 0b11,
            "column block function scalar did not round-trip through BG storage");
    require(std::addressof(function_value.type()) ==
                std::addressof(scalar_type_traits<Function>::type()) &&
                std::addressof(function_value.runtime_type()) ==
                    std::addressof(cpp_scalar_runtime_type<Function>()),
            "column block function provider did not return canonical type and runtime objects");
}

void test_erased_unary_and_mixed_column_callbacks() {
    using Unary = ColumnBlockFunction<i64, double>;
    using Mixed = ColumnBlockFunction<double, i64, double>;
    register_column_block_function<i64, double>();
    register_column_block_function<double, i64, double>();

    const auto rows = std::array{row_id_t{3}, row_id_t{8}, row_id_t{13}};
    const auto doubles = std::array{3.0, -4.0, 7.5};
    const auto integers = std::array<i64, 3>{2, 5, 9};
    const auto valid = std::array<std::uint8_t, 1>{0b101};
    const auto all_valid = std::array<std::uint8_t, 1>{0b111};
    const auto double_column = ColumnBatch<>{0, doubles.size(), doubles.data(),
                                              ColumnBatchValidity::from_bitmap(valid.data(), 0)};
    const auto integer_column = ColumnBatch<>{0, integers.size(), integers.data(),
                                               ColumnBatchValidity::from_bitmap(all_valid.data(), 0)};

    auto unary_value = ScalarValue<Unary>{Unary{&negate_batch}};
    const auto unary_view = unary_value.view();
    std::array<i64, 3> unary_output{};
    std::array<std::uint8_t, 1> unary_validity{};
    const auto unary_operands = std::array{ColumnOperandBatch<>{double_column}};
    const auto *unary_adapter = ScalarDispatch<ColumnBlockFunctionAdapter>::instance().find(unary_value.runtime_type());
    require(unary_adapter != nullptr, "unary column callback has no erased adapter");
    const auto unary_scalar = Scalar<>::from(unary_view);
    unary_adapter->call(unary_scalar, rows, unary_operands,
                        MutableColumnBatch<>{unary_output.data(), unary_output.size(), unary_validity});
    require(unary_output == std::array<i64, 3>{-3, 0, -7} && unary_validity[0] == 0b101,
            "erased unary callback did not preserve its result type or null values");

    auto mixed_value = ScalarValue<Mixed>{Mixed{&add_offset_batch}};
    const auto mixed_view = mixed_value.view();
    const auto offset = 0.5;
    const auto mixed_operands = std::array{ColumnOperandBatch<>{integer_column},
                                            ColumnOperandBatch<>{std::addressof(offset)}};
    std::array<double, 3> mixed_output{};
    std::array<std::uint8_t, 1> mixed_validity{};
    const auto *mixed_adapter = ScalarDispatch<ColumnBlockFunctionAdapter>::instance().find(mixed_value.runtime_type());
    require(mixed_adapter != nullptr, "mixed column callback has no erased adapter");
    const auto mixed_scalar = Scalar<>::from(mixed_view);
    mixed_adapter->call(mixed_scalar, rows, mixed_operands,
                        MutableColumnBatch<>{mixed_output.data(), mixed_output.size(), mixed_validity});
    require(mixed_output == std::array<double, 3>{2.5, 5.5, 9.5} && mixed_validity[0] == 0b111,
            "erased mixed callback did not adapt a heterogeneous column and scalar broadcast");
}

void test_column_expression_builder_integration() {
    register_column_block_function<i64, i64, i64>();
    auto unary_observations = std::vector<std::vector<std::optional<i64>>>{};
    const auto write_double = node([](In<TS<double>> input, Out<TSC<double>> output) { output.add(*input); });
    const auto observe_integer = node([&](In<TSC<i64>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto cells = std::vector<std::optional<i64>>{};
        input.for_each_row([&](row_id_t id, std::size_t) { cells.push_back(input.cell(id)); });
        unary_observations.push_back(std::move(cells));
        output.set(*tick);
    });
    const auto unary_graph = [&](wp<TS<double>> value, wp<TS<i64>> tick) {
        const auto column = write_double(value);
        const auto result = make_column_expression(ColumnBlockFunction<i64, double>{&negate_batch}, column);
        return observe_integer(result, tick);
    };
    const auto unary_values = std::array{3.0, -4.0};
    const auto unary_ticks = std::array<i64, 2>{0, 1};
    static_cast<void>(testing::test_graph(unary_graph, unary_values, unary_ticks));
    require(unary_observations ==
                std::vector<std::vector<std::optional<i64>>>{{-3}, {-3, 4}},
            "shared column expression builder did not materialize a unary operation with a different result type");

    auto mixed_observations = std::vector<std::vector<std::optional<double>>>{};
    const auto write_integer = node([](In<TS<i64>> input, Out<TSC<i64>> output) { output.add(*input); });
    const auto observe_double = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto cells = std::vector<std::optional<double>>{};
        input.for_each_row([&](row_id_t id, std::size_t) { cells.push_back(input.cell(id)); });
        mixed_observations.push_back(std::move(cells));
        output.set(*tick);
    });
    const auto mixed_graph = [&](wp<TS<i64>> value, wp<TS<i64>> integer_parameter,
                                 wp<TS<double>> double_parameter, wp<TS<i64>> tick) {
        const auto column = write_integer(value);
        const auto integer_result = make_column_expression(
            ColumnBlockFunction<i64, i64, i64>{&add_batch}, column, integer_parameter);
        const auto double_result = make_column_expression(
            ColumnBlockFunction<double, i64, double>{&add_offset_batch}, integer_result, double_parameter);
        return observe_double(double_result, tick);
    };
    const auto mixed_values = std::array<i64, 3>{2, 4, 6};
    const auto integer_parameters = std::array<i64, 3>{10, 20, 30};
    const auto double_parameters = std::array{0.5, 1.25, 2.0};
    const auto mixed_ticks = std::array<i64, 3>{0, 1, 2};
    static_cast<void>(testing::test_graph(mixed_graph, mixed_values, integer_parameters, double_parameters, mixed_ticks));
    require(mixed_observations == std::vector<std::vector<std::optional<double>>>{{12.5}, {23.25, 25.25},
                                                                                    {34.0, 36.0, 38.0}},
            "shared builder lost heterogeneous columns or parameter snapshots");
}

void test_deferred_column_union_matches_expression_form() {
    const auto source = node([](Out<TSC<double>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto column = source();
    const auto deferred = operators::add(column, column);
    const auto signature = Signature{util::symbol{"frame_probe"},
                                     {{util::symbol{"operand"}, &ProbeForm::type()}}};
    const auto arguments = std::array{WiringArgument{deferred}};
    const auto matches = signature.match_wiring_arguments(arguments);

    auto expression_matched = false;
    for (const auto &match : matches) {
        const auto *offered = match.argument_forms[0].offered_type;
        if (dynamic_cast<const TimeSeriesBundleType *>(offered) == nullptr)
            continue;

        expression_matched = true;
        require(&ProbeForm::type().resolve(match.type_map) == offered,
                "the Union did not capture the selected expression form");
    }
    require(expression_matched, "a deferred column did not match its expression form through a Union");
}

void test_column_expression_requires_a_column_operand() {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto scalar = operators::const_(3.0).as<TS<double>>();
    const auto result = make_column_expression(ColumnBlockFunction<i64, double>{&negate_batch}, scalar);

    auto rejected_scalar_only_expression = false;
    try {
        static_cast<void>(wiring.complete_pending_wiring(static_cast<WiringPoint>(result)));
    } catch (const WiringError &error) {
        rejected_scalar_only_expression =
            std::string_view{error.what()}.find("at least one column operand") != std::string_view::npos;
    }
    require(rejected_scalar_only_expression, "scalar-only column expression did not report its missing row domain");
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
                    std::addressof(TS<ColumnBlockFunction<i64, i64, i64>, TimeSeriesGuarantee::const_>::type()),
            "binary expression bundle did not retain its declared operand and compute types");

    static_cast<void>(time_series_runtime_traits<Type>::get(type));
}

void test_static_frame_expression_binding() {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    auto source = wp<TSF<QuoteFrame>>{wiring.borrowed_input(TSF<QuoteFrame>::type())};

    const auto result = bind_expression(source, col("bid") + col("ask"));
    require(std::addressof(result.type()) == std::addressof(TSC<double>::type()),
            "static frame expression did not bind its named columns");

    const auto parameterized = bind_expression(source, col("bid") + param("other"),
                                               "other"_n = select_column<double>(source, util::symbol{"ask"}));
    require(std::addressof(parameterized.type()) == std::addressof(TSC<double>::type()),
            "named expression parameter did not bind to a frame column");

    const auto difference = bind_expression(source, col("bid") - col("ask"));
    require(std::addressof(difference.type()) == std::addressof(TSC<double>::type()),
            "column subtraction did not bind its named columns");
    const auto parameterized_difference = bind_expression(
        source, param("other") - col("bid"), "other"_n = select_column<double>(source, util::symbol{"ask"}));
    require(std::addressof(parameterized_difference.type()) == std::addressof(TSC<double>::type()),
            "subtraction did not bind a captured parameter on the left side");
    const auto scalar_difference = bind_expression(source, 20.0 - col("bid"));
    require(std::addressof(scalar_difference.type()) == std::addressof(TSC<double>::type()),
            "subtraction did not capture a scalar on the left side");

    const auto missing_column = [&] {
        static_cast<void>(bind_expression(source, col("missing")));
    };
    auto missing_column_error = false;
    try {
        missing_column();
    } catch (const WiringError &error) {
        missing_column_error = std::string_view{error.what()}.find("missing") != std::string_view::npos &&
                               !error.contexts().empty();
    }
    require(missing_column_error, "missing frame column did not report its name and resolver context");
}

void test_runtime_schema_expression_binding() {
    const auto fields = std::array{SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &schema = anonymous_schema(fields);
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    auto source = wp<TSF<SCHEMA>>{wiring.borrowed_input(frame_series_type(schema, TimeSeriesGuarantee::ordinary))};

    const auto result = bind_expression(source, col("price"));
    require(std::addressof(result.type()) == std::addressof(TSC<double>::type()),
            "runtime schema expression did not resolve its named column");
}

void test_expression_bindings_are_per_call_and_owned() {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto first = wp<TSC<double>>{wiring.borrowed_input(TSC<double>::type())};
    const auto second = wp<TSC<double>>{wiring.borrowed_input(TSC<double>::type())};
    const auto integer = wp<TSC<i64>>{wiring.borrowed_input(TSC<i64>::type())};
    const auto expression = col("value");

    const auto first_result = bind_expression(expression, "value"_n = first);
    const auto second_result = bind_expression(expression, "value"_n = second);
    const auto integer_result = bind_expression(expression, "value"_n = integer);
    const auto first_result_point = static_cast<WiringPoint>(first_result);
    const auto second_result_point = static_cast<WiringPoint>(second_result);
    require(first_result_point.record() == static_cast<WiringPoint>(first).record() &&
                second_result_point.record() == static_cast<WiringPoint>(second).record() &&
                first_result_point.record() != second_result_point.record(),
            "expression reuse did not resolve each binding independently");
    require(std::addressof(integer_result.type()) == std::addressof(TSC<i64>::type()) &&
                std::addressof(first_result.type()) == std::addressof(TSC<double>::type()),
            "expression reuse did not preserve each binding's runtime type");

    const auto captured = capture(WiringArgument{first});
    const auto captured_result = bind_expression(captured);
    require(static_cast<WiringPoint>(captured_result).record() == static_cast<WiringPoint>(first).record(),
            "captured column did not retain its wiring point");

    auto foreign_wiring = GraphWiring{};
    auto foreign_point = WiringPoint{};
    {
        auto foreign_context = WiringContext{foreign_wiring};
        foreign_point = foreign_wiring.borrowed_input(TSC<double>::type());
    }
    auto rejected_foreign_point = false;
    try {
        static_cast<void>(bind_expression(capture(foreign_point)));
    } catch (const WiringError &error) {
        rejected_foreign_point = std::string_view{error.what()}.find("another GraphWiring") !=
                                 std::string_view::npos;
    }
    require(rejected_foreign_point, "expression binding accepted a captured point from another GraphWiring");
}

void test_expression_binding_errors_name_the_binding() {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto input = wp<TSC<double>>{wiring.borrowed_input(TSC<double>::type())};

    auto missing_parameter_named = false;
    try {
        static_cast<void>(bind_expression(param("threshold")));
    } catch (const WiringError &error) {
        missing_parameter_named = std::string_view{error.what()}.find("threshold") != std::string_view::npos;
    }
    require(missing_parameter_named, "missing parameter error did not name the parameter");

    auto duplicate_named = false;
    try {
        static_cast<void>(bind_expression(col("value"), "value"_n = input, "value"_n = input));
    } catch (const WiringError &error) {
        duplicate_named = std::string_view{error.what()}.find("value") != std::string_view::npos;
    }
    require(duplicate_named, "duplicate binding error did not name the binding");

    auto unused_named = false;
    try {
        static_cast<void>(bind_expression(col("value"), "value"_n = input, "unused"_n = input));
    } catch (const WiringError &error) {
        unused_named = std::string_view{error.what()}.find("unused") != std::string_view::npos;
    }
    require(unused_named, "unused binding error did not name the binding");
}

void test_non_inline_scalar_capture_owns_its_value() {
    const auto add_label_size = node([](std::string label, In<TS<i64>> tick, Out<TS<i64>> output) {
        output.set(*tick + static_cast<i64>(label.size()));
    });
    const auto graph = [&](wp<TS<i64>> tick) {
        const auto expression = [&] {
            const auto source = ScalarValue{std::string{"captured string"}};
            const auto operands = std::array{capture(source), capture(tick)};
            return bg::frame::apply(add_label_size, operands);
        }();
        return bind_expression(expression);
    };

    const auto ticks = std::array<i64, 3>{1, 2, 3};
    const auto actual = testing::test_graph(graph, ticks);
    require(actual.equals<i64>({16, 17, 18}),
            "a non-inline scalar capture did not survive binding and graph evaluation");
}

} // namespace

int main() {
    try {
        test_column_block_function_provider();
        test_erased_unary_and_mixed_column_callbacks();
        test_column_expression_builder_integration();
        test_deferred_column_union_matches_expression_form();
        test_column_expression_requires_a_column_operand();
        test_binary_expression_bundle_schema();
        test_static_frame_expression_binding();
        test_runtime_schema_expression_binding();
        test_expression_bindings_are_per_call_and_owned();
        test_expression_binding_errors_name_the_binding();
        test_non_inline_scalar_capture_owns_its_value();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
