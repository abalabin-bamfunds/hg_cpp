#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/library.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/util/named_argument.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct QuoteFrame {
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field> struct ComparisonFrame {
    Field<i64> size;
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field> struct TextComparisonFrame {
    Field<string> text;
    Field<symbol> venue;
};

template <template <typename, auto...> typename Field> struct NumericFrame {
    Field<i64> id;
    Field<i64> value;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field> struct MixedPredicateSchema {
    Field<TS<Expression, TimeSeriesGuarantee::const_>> expression;
    Field<TimeSeriesVariable<"FRAME_MIXED_LEFT">> binding_0;
    Field<TimeSeriesVariable<"FRAME_MIXED_RIGHT">> binding_1;
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

void greater_than_batch(std::span<const row_id_t> ids, const ColumnOperandBatch<i64> &lhs,
                        const ColumnOperandBatch<i64> &rhs, MutableColumnBatch<bool> output) {
    if (ids.size() != output.size())
        throw std::runtime_error{"boolean callback batch has an inconsistent size"};
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        const auto valid = lhs.is_valid(position) && rhs.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.set_value(position, lhs.value(position) > rhs.value(position));
    }
}

void copy_symbol_batch(std::span<const row_id_t> ids, const ColumnOperandBatch<symbol> &input,
                       MutableColumnBatch<symbol> output) {
    for (auto position = std::size_t{}; position != ids.size(); ++position) {
        const auto valid = input.is_valid(position);
        output.set_valid(position, valid);
        if (valid)
            output.set_value(position, input.value(position));
    }
}

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

template <cell_type T> struct ComparisonRowsState {
    std::array<row_id_t, 260> ids{};
};

template <cell_type T> struct comparison_rows_writer {
    In<TS<i64>> tick;
    State<ComparisonRowsState<T>> state;
    Out<TSC<T>> output;

    void evaluate() const {
        if (*tick == 0) {
            for (auto row = std::size_t{}; row != state->ids.size(); ++row) {
                const auto null_row = row == 7 || row == 8 || row == 127 || row == 128 || row == 255 || row == 256;
                const auto value = null_row ? std::optional<T>{}
                                            : std::optional<T>{static_cast<T>(static_cast<i64>(row) - 130)};
                state->ids[row] = output.add(value);
            }
            return;
        }

        output.set(state->ids[128], T{0});
        output.set(state->ids[129], std::optional<T>{});
        require(output.erase(state->ids[254]), "comparison source could not remove a row");
        output.add(T{1});
    }
};

void test_packed_boolean_expression_result() {
    register_column_block_function<bool, i64, i64>();

    const auto values_source = node([](In<TS<i64>> tick, Out<TSC<i64>> output) {
        if (*tick == 0)
            output.apply({3, 1, 5, 4, 9, 0, 7, 8, 4, 10, NULL_VALUE});
    });
    const auto threshold_source = node([](In<TS<i64>> tick, Out<TS<i64>> output) {
        if (*tick == 0)
            output.set(5);
    });

    auto observed = std::vector<std::optional<bool>>{};
    auto changed = std::vector<row_id_t>{};
    const auto observe = node([&](In<TSC<bool>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        input.for_each_row([&](row_id_t id, std::size_t) { observed.push_back(input.cell(id)); });
        input.for_each_changed([&](row_id_t id) { changed.push_back(id); });
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> tick) {
        const auto values = values_source(tick);
        const auto threshold = threshold_source(tick);
        return observe(
            make_column_expression(ColumnBlockFunction<bool, i64, i64>{&greater_than_batch}, values, threshold), tick);
    };
    const auto result = testing::test_graph(graph, std::array<i64, 1>{0});
    require(result.equals({0}), "packed boolean expression graph lost its trigger");
    require(observed == std::vector<std::optional<bool>>{false, false, false, false, true, false,
                                                         true, true, false, true, std::nullopt} &&
                changed == std::vector<row_id_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
            "packed boolean expression result lost values, nulls, or row changes");
}

void test_column_block_function_provider() {
    using Function = ColumnBlockFunction<i64, i64, i64>;

    auto function_value = ScalarValue<Function>{Function{&add_batch}};
    const auto function = function_value.view().value();

    const std::array ids{row_id_t{7}, row_id_t{12}};
    const std::array left_values{i64{2}, i64{10}};
    const std::array right_values{i64{4}, i64{3}};
    const auto left = ColumnBatch<i64>{0, left_values.size(), left_values.data(),
                                       ColumnBatch<i64>::Validity::all_valid()};
    const auto right = ColumnBatch<i64>{0, right_values.size(), right_values.data(),
                                        ColumnBatch<i64>::Validity::all_valid()};
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
    const auto double_column = ColumnBatch<>{0, doubles.size(), doubles.data(), 0, sizeof(double) * 8,
                                              ColumnBatchValidity::from_bitmap(valid.data(), 0)};
    const auto integer_column = ColumnBatch<>{0, integers.size(), integers.data(), 0, sizeof(i64) * 8,
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
    static_cast<void>(testing::test_graph(mixed_graph, mixed_values, integer_parameters, double_parameters,
                                          mixed_ticks));
    require(mixed_observations == std::vector<std::vector<std::optional<double>>>{{12.5}, {23.25, 25.25},
                                                                                    {34.0, 36.0, 38.0}},
            "shared builder lost heterogeneous columns or parameter snapshots");
}

void test_symbol_producing_column_block_function() {
    register_column_block_function<symbol, symbol>();
    auto observations = std::vector<std::vector<std::optional<symbol>>>{};
    const auto write = node([](In<TS<i64>> tick, Out<TSC<symbol>> output) {
        if (*tick != 0)
            return;
        output.add(symbol{"XNAS"});
        output.add(symbol{});
        output.add(symbol{""});
        output.add(std::nullopt);
    });
    const auto observe = node([&](In<TSC<symbol>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto cells = std::vector<std::optional<symbol>>{};
        input.for_each_row([&](row_id_t id, std::size_t) { cells.push_back(input.cell(id)); });
        observations.push_back(std::move(cells));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> tick) {
        const auto copied = make_column_expression(ColumnBlockFunction<symbol, symbol>{&copy_symbol_batch},
                                                   write(tick));
        return observe(copied, tick);
    };

    const auto result = testing::test_graph(graph, std::array<i64, 2>{0, 1});
    require(result.equals<i64>({0, 1}) && observations.size() == 2 &&
                observations[0] == std::vector<std::optional<symbol>>{symbol{"XNAS"}, symbol{}, symbol{""},
                                                                      std::nullopt} &&
                observations[1] == observations[0],
            "a symbol-producing column callback changed missing symbols, interned empty symbols, or null cells");
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
    static_assert(std::same_as<decltype(convert<i64>(col("bid"))), Expression>);

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

    const auto converted_bound_column = operators::convert[type<TSC<i64>>](bind_expression(source, col("bid")));
    require(std::addressof(converted_bound_column.type()) == std::addressof(TSC<i64>::type()),
            "column conversion did not accept an erased bound expression result");
    const auto bound_column = bind_expression(source, col("bid"));
    const auto identity_bound_column = operators::convert[type<TSC<double>>](bound_column);
    require(std::addressof(identity_bound_column.type()) == std::addressof(TSC<double>::type()) &&
                static_cast<WiringPoint>(identity_bound_column).record() ==
                    static_cast<WiringPoint>(bound_column).record(),
            "identity conversion did not preserve an erased bound column");
    const auto deferred_column = bind_expression(source, col("bid") + col("ask"));
    const auto identity_deferred_column = operators::convert[type<TSC<double>>](deferred_column);
    require(std::addressof(identity_deferred_column.type()) == std::addressof(TSC<double>::type()) &&
                static_cast<WiringPoint>(identity_deferred_column).record() ==
                    static_cast<WiringPoint>(deferred_column).record(),
            "identity conversion did not preserve a deferred column expression");

    const auto converted_expression = bind_expression(source, frame::convert<i64>(col("bid")));
    require(std::addressof(converted_expression.type()) == std::addressof(TSC<i64>::type()),
            "unbound column conversion did not retain its requested output type");

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

void test_numeric_comparison_expression_wiring() {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto source = wp<TSF<ComparisonFrame>>{wiring.borrowed_input(TSF<ComparisonFrame>::type())};

    const auto integer_expressions = std::array{
        col("size") == col("size"), col("size") != col("size"), col("size") < col("size"),
        col("size") <= col("size"), col("size") > col("size"), col("size") >= col("size"),
        col("size") == i64{10}, col("size") != i64{10}, col("size") < i64{10},
        col("size") <= i64{10}, col("size") > i64{10}, col("size") >= i64{10},
        i64{10} == col("size"), i64{10} != col("size"), i64{10} < col("size"),
        i64{10} <= col("size"), i64{10} > col("size"), i64{10} >= col("size"),
    };
    for (const auto &expression : integer_expressions) {
        const auto result = bind_expression(source, expression);
        require(std::addressof(result.type()) == std::addressof(TSC<bool>::type()),
                "integer comparison expression did not produce TSC<bool>");
    }

    const auto double_expressions = std::array{
        col("ask") == col("bid"), col("ask") != col("bid"), col("ask") < col("bid"),
        col("ask") <= col("bid"), col("ask") > col("bid"), col("ask") >= col("bid"),
        col("ask") == 10.0, col("ask") != 10.0, col("ask") < 10.0,
        col("ask") <= 10.0, col("ask") > 10.0, col("ask") >= 10.0,
        10.0 == col("ask"), 10.0 != col("ask"), 10.0 < col("ask"),
        10.0 <= col("ask"), 10.0 > col("ask"), 10.0 >= col("ask"),
    };
    for (const auto &expression : double_expressions) {
        const auto result = bind_expression(source, expression);
        require(std::addressof(result.type()) == std::addressof(TSC<bool>::type()),
                "double comparison expression did not produce TSC<bool>");
    }

    const auto integer_product = bind_expression(source, col("size") * col("size"));
    const auto integer_quotient = bind_expression(source, col("size") / col("size"));
    require(std::addressof(integer_product.type()) == std::addressof(TSC<i64>::type()) &&
                std::addressof(integer_quotient.type()) == std::addressof(TSC<double>::type()),
            "integer multiplication or division expression produced the wrong column type");

    const auto tolerance = wp<TS<double>>{wiring.borrowed_input(TS<double>::type())};
    const auto captured_predicate = col("ask") - col("bid") < tolerance;
    const auto captured_result = bind_expression(source, captured_predicate);
    const auto named_predicate = col("ask") - col("bid") < param("tolerance");
    const auto named_result = bind_expression(source, named_predicate, "tolerance"_n = tolerance);
    require(std::addressof(captured_result.type()) == std::addressof(TSC<bool>::type()) &&
                std::addressof(named_result.type()) == std::addressof(TSC<bool>::type()),
            "comparison expression did not capture its TS operand or bind its named parameter");

    auto rejected_mixed_types = false;
    try {
        static_cast<void>(bind_expression(source, col("size") < col("ask")));
    } catch (const WiringError &) {
        rejected_mixed_types = true;
    }
    require(rejected_mixed_types, "comparison expression accepted implicit numeric promotion");

    auto rejected_numeric_filter = false;
    try {
        static_cast<void>(frame::filter(source, col("bid") + col("ask")));
    } catch (const WiringError &) {
        rejected_numeric_filter = true;
    }
    require(rejected_numeric_filter, "frame filter accepted a numeric predicate");

    const auto bound_predicate = wp<>{wiring.borrowed_input(TSC<bool>::type())};
    const auto filtered_column_predicate = frame::filter(source, bound_predicate);
    require(std::addressof(filtered_column_predicate.type()) == std::addressof(TSF<ComparisonFrame>::type()),
            "frame filter did not accept a bound TSC<bool> predicate");
    auto rejected_bindings_for_bound_predicate = false;
    try {
        static_cast<void>(frame::filter(source, bound_predicate, "unused"_n = true));
    } catch (const WiringError &) {
        rejected_bindings_for_bound_predicate = true;
    }
    require(rejected_bindings_for_bound_predicate,
            "frame filter accepted expression bindings for an already bound predicate");

    const auto bound_series_predicate = wp<>{wiring.borrowed_input(TS<bool>::type())};
    const auto filtered_series_predicate = frame::filter(source, bound_series_predicate);
    require(std::addressof(filtered_series_predicate.type()) == std::addressof(TSF<ComparisonFrame>::type()),
            "frame filter did not accept a bound TS<bool> predicate");
}

void test_filter_expression_with_multiple_parameters() {
    using namespace bg;
    using namespace bg::frame;

    auto selected_counts = std::vector<std::size_t>{};
    const auto observe = node([&](In<TSF<ComparisonFrame>> input, Out<TS<i64>> output) {
        auto count = std::size_t{};
        input.for_each_row([&](row_id_t, std::size_t) { ++count; });
        selected_counts.push_back(count);
        output.set(static_cast<i64>(count));
    });
    const auto graph = [&](wp<TSF<ComparisonFrame>> source, wp<TS<double>> first, wp<TS<double>> second,
                           wp<TS<double>> third, wp<TS<double>> fourth, wp<TS<double>> fifth) {
        const auto predicate =
            col("ask") + first + second + third + fourth + param("fifth") > 130.0;
        return observe(frame::filter(source, predicate, "fifth"_n = fifth));
    };

    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<ComparisonFrame>::type(),
                       {literal::TimeSeriesLiteral{}, {{{"size", i64{1}}, {"bid", 10.0}, {"ask", 100.0}}},
                        literal::TimeSeriesLiteral{}}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {1.0, 1.0, 1.0}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {2.0, 2.0, 2.0}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {4.0, 4.0, 4.0}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {8.0, 8.0, 8.0}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {16.0, 16.0, 15.0}));
    require(result.equals<i64>({0, 1, 0}) && selected_counts == std::vector<std::size_t>{0, 1, 0},
            "frame filter did not evaluate parameters stored across multiple aligned blocks");
}

void test_mixed_expression_chaining_and_filter() {
    const auto value_plus_one = col("value") + i64{1};
    const auto threshold = param("binding_0");

    auto selected_counts = std::vector<std::size_t>{};
    auto first_adjusted_values = std::vector<std::vector<std::optional<double>>>{};
    auto second_adjusted_values = std::vector<std::vector<std::optional<double>>>{};
    const auto observe = node([&](In<TSF<NumericFrame>> input, In<TSC<double>> first_adjusted,
                                  In<TSC<double>> second_adjusted, Out<TS<i64>> output) {
        auto count = std::size_t{};
        input.for_each_row([&](row_id_t, std::size_t) { ++count; });
        selected_counts.push_back(count);
        auto first_values = std::vector<std::optional<double>>{};
        first_adjusted.for_each_row([&](row_id_t id, std::size_t) { first_values.push_back(first_adjusted.cell(id)); });
        first_adjusted_values.push_back(std::move(first_values));
        auto second_values = std::vector<std::optional<double>>{};
        second_adjusted.for_each_row(
            [&](row_id_t id, std::size_t) { second_values.push_back(second_adjusted.cell(id)); });
        second_adjusted_values.push_back(std::move(second_values));
        output.set(static_cast<i64>(count));
    });
    const auto graph = [&](wp<TSF<NumericFrame>> source, wp<TS<i64>> offset, wp<TS<i64>> limit,
                           wp<TS<bool>> gate, wp<TS<double>> first_adjustment,
                           wp<TS<double>> second_adjustment, wp<TS<i64>> tick) {
        const auto mixed_value = value_plus_one + offset;
        const auto predicate = mixed_value > threshold;
        const auto reversed = bind_expression(source, offset - value_plus_one);
        require(std::addressof(reversed.type()) == std::addressof(TSC<i64>::type()),
                "reversed mixed operands did not bind to a column result");
        const auto inverted = wp<TSB<MixedPredicateSchema>>{!predicate && gate};
        const auto converted_value = convert<double>(col("value"));
        const auto first_adjusted = bind_expression(source, converted_value + first_adjustment);
        const auto second_adjusted = bind_expression(source, converted_value + second_adjustment);
        return observe(frame::filter(source, inverted, "binding_0"_n = limit), first_adjusted, second_adjusted);
    };

    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<NumericFrame>::type(),
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{1}}, {"value", i64{2}}, {"price", 10.0}},
                         {{"id", i64{2}}, {"value", i64{4}}, {"price", 20.0}}},
                        {{{"id", i64{1}}, {"value", i64{6}}}}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {2, 2, 2}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {6, 6, 8}),
        testing::input(cpp_time_series_runtime_type<TS<bool>>(), {true, true, true}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {1.5, 1.5, 1.5}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {-0.5, -0.5, -0.5}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1, 2}));
    require(result.equals<i64>({0, 1, 1}) && selected_counts == std::vector<std::size_t>{0, 1, 1},
            "mixed expression chaining or filtering lost bound values");
    require(first_adjusted_values ==
                std::vector<std::vector<std::optional<double>>>{{}, {3.5, 5.5}, {7.5, 5.5}} &&
                second_adjusted_values ==
                    std::vector<std::vector<std::optional<double>>>{{}, {1.5, 3.5}, {5.5, 3.5}},
            "converted expression reuse did not resolve each mixed binding independently");
}

void test_mixed_series_bindings_keep_independent_types() {
    const auto integer_source = node([](Out<TS<i64>>) {});
    const auto double_source = node([](Out<TS<double>>) {});
    const auto boolean_source = node([](Out<TS<bool>>) {});
    const auto frame_source = node([](Out<TSF<NumericFrame>>) {});

    testing::test_wiring([&] {
        const auto integer_capture = integer_source();
        const auto double_capture = double_source();
        const auto integer_mixed = col("value") + integer_capture;
        const auto double_mixed = col("price") + double_capture;

        const auto *integer_type = dynamic_cast<const TimeSeriesBundleType *>(&integer_mixed.type());
        const auto *double_type = dynamic_cast<const TimeSeriesBundleType *>(&double_mixed.type());
        require(integer_type != nullptr && double_type != nullptr && integer_type->item_count() == 2 &&
                    double_type->item_count() == 2,
                "mixed numeric operands did not retain their binding schemas");
        require(std::addressof(integer_type->item_type(1)) == std::addressof(TS<i64>::type()) &&
                    std::addressof(double_type->item_type(1)) == std::addressof(TS<double>::type()),
                "mixed operands did not retain their independent capture types");

        const auto predicate = (integer_mixed > i64{0}) && (double_mixed > 0.0);
        const auto bound_predicate = bind_expression(frame_source(), predicate);
        require(std::addressof(bound_predicate.type()) == std::addressof(TSC<bool>::type()),
                "binding a mixed predicate with heterogeneous captures failed");

        const auto native_sum = operators::add(integer_capture, integer_source());
        require(std::addressof(native_sum.type()) == std::addressof(TS<i64>::type()),
                "native TS<i64> addition did not resolve to the core operator");

        auto rejected_boolean_numeric_addition = false;
        try {
            static_cast<void>(operators::add(boolean_source(), integer_capture));
        } catch (const WiringError &) {
            rejected_boolean_numeric_addition = true;
        }
        require(rejected_boolean_numeric_addition, "boolean and numeric time series were accepted by addition");
    });
}

void test_mixed_operator_requirements() {
    struct observation {
        std::vector<std::optional<i64>> after_constant;
        std::vector<std::optional<i64>> before_constant;
        std::vector<std::optional<i64>> mixed_sum;
        std::vector<std::optional<bool>> disjunction;
    };
    auto observations = std::vector<observation>{};
    const auto observe = node([&](In<TSC<i64>> after_constant, In<TSC<i64>> before_constant,
                                  In<TSC<i64>> mixed_sum, In<TSC<bool>> disjunction, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto result = observation{};
        after_constant.for_each_row(
            [&](row_id_t id, std::size_t) { result.after_constant.push_back(after_constant.cell(id)); });
        before_constant.for_each_row(
            [&](row_id_t id, std::size_t) { result.before_constant.push_back(before_constant.cell(id)); });
        mixed_sum.for_each_row([&](row_id_t id, std::size_t) { result.mixed_sum.push_back(mixed_sum.cell(id)); });
        disjunction.for_each_row(
            [&](row_id_t id, std::size_t) { result.disjunction.push_back(disjunction.cell(id)); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<NumericFrame>> source, wp<TS<i64>> first_offset,
                           wp<TS<i64>> second_offset, wp<TS<i64>> tick) {
        const auto value_plus_one = col("value") + i64{1};
        const auto first_mixed = value_plus_one + first_offset;
        const auto second_mixed = value_plus_one + second_offset;
        const auto after_constant = first_mixed + i64{3};
        const auto before_constant = i64{3} + first_mixed;
        const auto mixed_sum = first_mixed + second_mixed;
        const auto disjunction = (first_mixed > i64{3}) || (second_mixed > i64{4});
        return observe(bind_expression(source, after_constant), bind_expression(source, before_constant),
                       bind_expression(source, mixed_sum), bind_expression(source, disjunction), tick);
    };
    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<NumericFrame>::type(),
                       {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"value", i64{2}}, {"price", 0.0}}}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {1, 1}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {2, 2}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1}));
    require(result.equals<i64>({0, 1}) && observations.size() == 2,
            "mixed operator observer missed an input cycle");
    require(observations[1].after_constant == std::vector<std::optional<i64>>{7} &&
                observations[1].before_constant == std::vector<std::optional<i64>>{7} &&
                observations[1].mixed_sum == std::vector<std::optional<i64>>{9} &&
                observations[1].disjunction == std::vector<std::optional<bool>>{true},
            "mixed operators lost constants, nested bindings, or disjunction results");
}

void test_mixed_expression_parameter_invalidation() {
    const auto value_plus_one = col("value") + i64{1};
    const auto threshold = param("binding_0");
    struct observation {
        bool valid{};
        bool became_invalid{};
        bool became_valid{};
        std::vector<std::optional<bool>> cells;
    };
    auto observations = std::vector<observation>{};

    const auto observe = node([&](In<TSC<bool>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(),
                                  .became_invalid = input.became_invalid(),
                                  .became_valid = input.became_valid()};
        input.for_each_row([&](row_id_t id, std::size_t) { result.cells.push_back(input.cell(id)); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<NumericFrame>> source, wp<TS<i64>> offset, wp<TS<i64>> limit,
                           wp<TS<i64>> tick) {
        const auto mixed_value = value_plus_one + offset;
        const auto predicate = mixed_value > threshold;
        return observe(opt(bind_expression(source, predicate, "binding_0"_n = limit)), tick);
    };
    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<NumericFrame>::type(),
                       {{{{"id", i64{1}}, {"value", i64{2}}, {"price", 10.0}}}, literal::SKIP,
                        literal::TimeSeriesLiteral{}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {2, literal::INVALID, 2}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {5, literal::SKIP, 6}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1, 2}));
    require(result.equals<i64>({0, 1, 2}) && observations.size() == 3,
            "mixed expression observer missed a parameter cycle");
    require(observations[0].valid && !observations[0].became_invalid && observations[0].became_valid &&
                observations[0].cells == std::vector<std::optional<bool>>{false},
            "mixed expression did not report its first valid publication");
    require(!observations[1].valid && observations[1].became_invalid,
            "mixed expression did not report parameter invalidation");
    require(!observations[2].became_invalid && observations[2].became_valid &&
                observations[2].cells == std::vector<std::optional<bool>>{false},
            "mixed expression did not report parameter revalidation");
}

template <cell_type T, typename Comparator, typename Operation>
void test_comparison_runtime(Operation &operation) {
    struct observation {
        std::map<row_id_t, std::optional<bool>> cells;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };

    auto observations = std::vector<observation>{};
    const auto observe = node([&](In<TSC<bool>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) { result.cells.emplace(id, input.cell(id)); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        std::ranges::sort(result.changed);
        std::ranges::sort(result.removed);
        observations.push_back(std::move(result));
        output.set(*tick);
    });

    const auto write = node<comparison_rows_writer<T>>();
    const auto graph = [&](wp<TS<i64>> tick) {
        return observe(operation(write(tick), T{0}), tick);
    };
    const auto ticks = std::array<i64, 2>{0, 1};
    const auto result = testing::test_graph(graph, ticks);
    require(result.equals({0, 1}) && observations.size() == ticks.size(),
            "numeric comparison did not evaluate both source ticks");

    auto expected_cells = std::map<row_id_t, std::optional<T>>{};
    for (auto row = row_id_t{}; row != 260; ++row) {
        const auto null_row = row == 7 || row == 8 || row == 127 || row == 128 || row == 255 || row == 256;
        expected_cells.emplace(row, null_row ? std::optional<T>{}
                                             : std::optional<T>{static_cast<T>(static_cast<i64>(row) - 130)});
    }

    const auto expected_snapshot = [&](const auto &cells) {
        auto result = std::map<row_id_t, std::optional<bool>>{};
        for (const auto &[id, value] : cells) {
            result.emplace(id, value ? std::optional<bool>{std::invoke(Comparator{}, *value, T{0})}
                                     : std::nullopt);
        }
        return result;
    };
    const auto initial = expected_snapshot(expected_cells);
    require(observations[0].cells == initial && observations[0].changed.size() == expected_cells.size() &&
                observations[0].removed.empty(),
            "numeric comparison lost initial rows, nulls, or additions");

    expected_cells[128] = T{0};
    expected_cells[129] = std::nullopt;
    expected_cells.erase(254);
    expected_cells.emplace(260, T{1});
    require(observations[1].cells == expected_snapshot(expected_cells) &&
                observations[1].changed == std::vector<row_id_t>{128, 129, 260} &&
                observations[1].removed == std::vector<row_id_t>{254},
            "numeric comparison lost sparse updates, removal, or the new row");
}

void test_text_and_symbol_comparison_filters() {
    struct observation {
        std::array<std::size_t, 5> counts;
        bool operator==(const observation &) const = default;
    };

    auto observations = std::vector<observation>{};
    const auto observe = node([&](In<TSF<TextComparisonFrame>> text_literal,
                                  In<TSF<TextComparisonFrame>> symbol_bound,
                                  In<TSF<TextComparisonFrame>> symbol_series,
                                  In<TSF<TextComparisonFrame>> text_series,
                                  In<TSF<TextComparisonFrame>> named_parameter, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto count_rows = [](const auto &input) {
            auto count = std::size_t{};
            input.for_each_row([&](row_id_t, std::size_t) { ++count; });
            return count;
        };
        observations.push_back({{count_rows(text_literal), count_rows(symbol_bound), count_rows(symbol_series),
                                 count_rows(text_series), count_rows(named_parameter)}});
        output.set(*tick);
    });

    const auto graph = [&](wp<TSF<TextComparisonFrame>> source, wp<TS<string>> text_parameter,
                           wp<TS<symbol>> symbol_parameter, wp<TS<i64>> tick) {
        const auto symbol_predicate = bind_expression(source, col("venue") != symbol{"XNAS"});
        const auto named_predicate = col("venue") == param("target");
        return observe(frame::filter(source, col("text") == std::string_view{"alpha"}),
                       frame::filter(source, symbol_predicate),
                       frame::filter(source, col("venue") == symbol_parameter),
                       frame::filter(source, col("text") == text_parameter),
                       frame::filter(source, named_predicate, "target"_n = symbol_parameter), tick);
    };

    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<TextComparisonFrame>::type(),
                       {literal::TimeSeriesLiteral{},
                        {{{"text", std::string_view{"alpha"}}, {"venue", symbol{"XNAS"}}},
                         {{"text", std::string_view{"beta"}}, {"venue", symbol{"XNYS"}}},
                         {{"text", NULL_VALUE}, {"venue", symbol{"XNAS"}}},
                         {{"text", std::string_view{}}, {"venue", symbol{""}}}},
                        {{{"text", std::string_view{"alpha"}}, {"venue", symbol{"XNYS"}}}}}),
        testing::input(cpp_time_series_runtime_type<TS<string>>(),
                       {std::string_view{"alpha"}, std::string_view{"beta"}, std::string_view{"alpha"}}),
        testing::input(cpp_time_series_runtime_type<TS<symbol>>(),
                       {symbol{"XNAS"}, symbol{"XNYS"}, symbol{"XNYS"}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1, 2}));

    require(result.equals<i64>({0, 1, 2}) &&
                observations == std::vector<observation>{{{0, 0, 0, 0, 0}}, {{1, 2, 1, 1, 1}}, {{2, 3, 2, 2, 2}}},
            "string or symbol comparison lost scalar, bound, time-series, null, empty, or filter behavior");
}

void test_comparison_expression_with_tick_parameter() {
    using namespace bg;
    using namespace bg::frame;

    const auto graph = [](wp<TSF<ComparisonFrame>> frame, wp<TS<double>> tolerance) {
        const auto predicate = col("ask") - col("bid") < tolerance;
        return bind_expression(frame, predicate);
    };
    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<ComparisonFrame>::type(),
                       {literal::TimeSeriesLiteral{}, {{{"size", i64{1}}, {"bid", 10.0}, {"ask", 13.0}}},
                        {{{"size", i64{2}}, {"bid", 20.0}, {"ask", 23.0}}}}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {2.0, 2.5, 4.0}));
    require(result.equals({literal::TimeSeriesLiteral{}, {{"first", false}}, {{"first", true}, {"second", true}}}),
            "comparison expression lost arithmetic fusion, parameter ticks, or row values");
}

void test_comparison_uses_forced_concrete_intermediate() {
    using namespace bg;
    using namespace bg::frame;

    auto differences = std::vector<std::vector<std::optional<double>>>{};
    const auto observe_difference = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto cells = std::vector<std::optional<double>>{};
        input.for_each_row([&](row_id_t id, std::size_t) { cells.push_back(input.cell(id)); });
        differences.push_back(std::move(cells));
        output.set(*tick);
    });

    auto predicates = std::vector<std::vector<std::optional<bool>>>{};
    const auto observe_predicate = node([&](In<TSC<bool>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto cells = std::vector<std::optional<bool>>{};
        input.for_each_row([&](row_id_t id, std::size_t) { cells.push_back(input.cell(id)); });
        predicates.push_back(std::move(cells));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<ComparisonFrame>> frame, wp<TS<double>> tolerance, wp<TS<i64>> tick) {
        const auto ask = select_column<double>(frame, util::symbol{"ask"});
        const auto bid = select_column<double>(frame, util::symbol{"bid"});
        const auto difference = operators::sub(ask, bid).as<TSC<double>>();
        static_cast<void>(observe_difference(difference, tick));
        return observe_predicate(operators::lt(difference, tolerance).as<TSC<bool>>(), tick);
    };

    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<ComparisonFrame>::type(),
                       {literal::TimeSeriesLiteral{}, {{{"size", i64{1}}, {"bid", 10.0}, {"ask", 13.0}}},
                        {{{"size", i64{2}}, {"bid", 20.0}, {"ask", 23.0}}}}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(), {2.0, 2.5, 4.0}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1, 2}));
    require(result.equals({0, 1, 2}) &&
                differences == std::vector<std::vector<std::optional<double>>>{{}, {3.0}, {3.0, 3.0}} &&
                predicates == std::vector<std::vector<std::optional<bool>>>{{}, {false}, {true, true}},
            "comparison did not reuse a forced concrete arithmetic intermediate");
}

void test_numeric_arithmetic_and_conversion_updates() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::vector<std::optional<i64>> sums;
        std::vector<std::optional<i64>> differences;
        std::vector<std::optional<i64>> products;
        std::vector<std::optional<double>> quotients;
        std::vector<std::optional<double>> double_products;
        std::vector<std::optional<double>> double_quotients;
        std::vector<std::optional<i64>> prices;
        std::vector<std::optional<double>> fused;
        std::vector<row_id_t> sum_changed;
        std::vector<row_id_t> difference_changed;
        std::vector<row_id_t> product_changed;
        std::vector<row_id_t> product_removed;
        std::vector<row_id_t> price_changed;
        std::vector<row_id_t> price_removed;
        std::vector<row_id_t> fused_changed;
        std::vector<row_id_t> fused_removed;
    };

    auto observations = std::vector<observation>{};
    const auto observe = node([&](In<TSC<i64>> sum, In<TSC<i64>> difference, In<TSC<i64>> product,
                                  In<TSC<double>> quotient, In<TSC<double>> double_product,
                                  In<TSC<double>> double_quotient, In<TSC<i64>> price,
                                  In<TSC<double>> fused, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        sum.for_each_row([&](row_id_t id, std::size_t) { result.sums.push_back(sum.cell(id)); });
        difference.for_each_row([&](row_id_t id, std::size_t) { result.differences.push_back(difference.cell(id)); });
        product.for_each_row([&](row_id_t id, std::size_t) { result.products.push_back(product.cell(id)); });
        quotient.for_each_row([&](row_id_t id, std::size_t) { result.quotients.push_back(quotient.cell(id)); });
        double_product.for_each_row(
            [&](row_id_t id, std::size_t) { result.double_products.push_back(double_product.cell(id)); });
        double_quotient.for_each_row(
            [&](row_id_t id, std::size_t) { result.double_quotients.push_back(double_quotient.cell(id)); });
        price.for_each_row([&](row_id_t id, std::size_t) { result.prices.push_back(price.cell(id)); });
        fused.for_each_row([&](row_id_t id, std::size_t) { result.fused.push_back(fused.cell(id)); });
        sum.for_each_changed([&](row_id_t id) { result.sum_changed.push_back(id); });
        difference.for_each_changed([&](row_id_t id) { result.difference_changed.push_back(id); });
        product.for_each_changed([&](row_id_t id) { result.product_changed.push_back(id); });
        product.for_each_removed([&](row_id_t id) { result.product_removed.push_back(id); });
        price.for_each_changed([&](row_id_t id) { result.price_changed.push_back(id); });
        price.for_each_removed([&](row_id_t id) { result.price_removed.push_back(id); });
        fused.for_each_changed([&](row_id_t id) { result.fused_changed.push_back(id); });
        fused.for_each_removed([&](row_id_t id) { result.fused_removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<NumericFrame>> source, wp<TS<double>> tolerance, wp<TS<i64>> tick) {
        const auto sum = bind_expression(source, col("value") + i64{1});
        const auto difference = bind_expression(source, col("value") - i64{1});
        const auto product = bind_expression(source, col("value") * i64{2});
        const auto quotient = bind_expression(source, col("value") / i64{-1});
        const auto double_product = bind_expression(source, col("price") * 2.0);
        const auto double_quotient = bind_expression(source, col("price") / 2.0);
        const auto price = bind_expression(source, convert<i64>(col("price")));
        const auto fused = bind_expression(source, convert<double>(col("value")) + tolerance);
        return observe(sum, difference, product, quotient, double_product, double_quotient, price, fused, tick);
    };

    const auto two_to_63 = std::ldexp(1.0, 63);
    const auto below_limit = std::nextafter(two_to_63, 0.0);
    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<NumericFrame>::type(),
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{1}}, {"value", std::numeric_limits<i64>::max()}, {"price", 2.9}}},
                        {{{"id", i64{2}}, {"value", std::numeric_limits<i64>::min()}, {"price", -2.9}}},
                        {{{"id", i64{2}}, {"price", -two_to_63}}},
                        {{{"id", i64{2}}, {"price", below_limit}}},
                        {literal::REMOVE({{"id", i64{1}}})}}),
        testing::input(cpp_time_series_runtime_type<TS<double>>(),
                       {4096.0, literal::SKIP, literal::SKIP, literal::SKIP, 8192.0, literal::SKIP}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1, 2, 3, 4, 5}));

    require(result.equals<i64>({0, 1, 2, 3, 4, 5}) && observations.size() == 6,
            "numeric arithmetic and conversion graph missed an input cycle");
    require(observations[1].products == std::vector<std::optional<i64>>{-2} &&
                observations[1].sums == std::vector<std::optional<i64>>{std::numeric_limits<i64>::min()} &&
                observations[1].differences ==
                    std::vector<std::optional<i64>>{std::numeric_limits<i64>::max() - 1} &&
                observations[1].quotients == std::vector<std::optional<double>>{-two_to_63} &&
                observations[1].double_products == std::vector<std::optional<double>>{5.8} &&
                observations[1].double_quotients == std::vector<std::optional<double>>{1.45} &&
                observations[1].prices == std::vector<std::optional<i64>>{2} &&
                observations[1].fused == std::vector<std::optional<double>>{two_to_63 + 4096.0} &&
                observations[1].product_changed == std::vector<row_id_t>{0},
            "integer arithmetic, double arithmetic, or explicit conversion produced an incorrect result");
    require(observations[2].products == std::vector<std::optional<i64>>{-2, 0} &&
                observations[2].sums == std::vector<std::optional<i64>>{std::numeric_limits<i64>::min(),
                                                                        std::numeric_limits<i64>::min() + 1} &&
                observations[2].differences == std::vector<std::optional<i64>>{
                                                   std::numeric_limits<i64>::max() - 1,
                                                   std::numeric_limits<i64>::max()} &&
                observations[2].quotients == std::vector<std::optional<double>>{-two_to_63, two_to_63} &&
                observations[2].double_products == std::vector<std::optional<double>>{5.8, -5.8} &&
                observations[2].double_quotients == std::vector<std::optional<double>>{1.45, -1.45} &&
                observations[2].prices == std::vector<std::optional<i64>>{2, -2} &&
                observations[2].fused == std::vector<std::optional<double>>{two_to_63 + 4096.0,
                                                                           -two_to_63 + 4096.0} &&
                observations[2].product_changed == std::vector<row_id_t>{1},
            "numeric operators failed at signed boundaries or when conversion captured a scalar TS");
    require(observations[3].products == observations[2].products && observations[3].product_changed.empty() &&
                observations[3].sum_changed.empty() && observations[3].difference_changed.empty() &&
                observations[3].fused_changed.empty() &&
                observations[3].prices == std::vector<std::optional<i64>>{2, std::numeric_limits<i64>::min()} &&
                observations[3].price_changed == std::vector<row_id_t>{1},
            "a price-only patch recomputed unrelated arithmetic or rejected the inclusive lower boundary");
    require(observations[4].prices == std::vector<std::optional<i64>>{2, static_cast<i64>(below_limit)} &&
                observations[4].price_changed == std::vector<row_id_t>{1} &&
                observations[4].fused == std::vector<std::optional<double>>{two_to_63 + 8192.0,
                                                                           -two_to_63 + 8192.0} &&
                observations[4].fused_changed == std::vector<row_id_t>{0, 1},
            "conversion or fused scalar capture failed at the upper i64 boundary");
    require(observations[5].product_removed == std::vector<row_id_t>{0} &&
                observations[5].price_removed == std::vector<row_id_t>{0} &&
                observations[5].fused_removed == std::vector<row_id_t>{0},
            "arithmetic or conversion did not propagate a keyed row removal");
}

void test_numeric_null_and_range_errors() {
    using namespace bg;
    using namespace bg::frame;

    auto values = std::vector<std::optional<double>>{};
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        values.clear();
        input.for_each_row([&](row_id_t id, std::size_t) { values.push_back(input.cell(id)); });
        output.set(*tick);
    });
    const auto nullable_division = [&](wp<TSF<NumericFrame>> source, wp<TS<i64>> tick) {
        return observe(bind_expression(source, col("value") / i64{0}), tick);
    };
    const auto null_result = testing::test_graph(
        nullable_division,
        testing::input(TSF<NumericFrame>::type(),
                       {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"price", 1.0}}}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {0, 1}));
    require(null_result.equals<i64>({0, 1}) && values == std::vector<std::optional<double>>{std::nullopt},
            "a null numerator evaluated division by zero instead of remaining null");

    const auto rejects_conversion = [&](double value, bool domain_error) {
        const auto convert_graph = [](wp<TSF<NumericFrame>> source) {
            return bind_expression(source, convert<i64>(col("price")));
        };
        auto rejected = false;
        try {
            static_cast<void>(testing::test_graph(
                convert_graph,
                testing::input(TSF<NumericFrame>::type(),
                               {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"price", value}}}})));
        } catch (const std::domain_error &) {
            rejected = domain_error;
        } catch (const std::overflow_error &) {
            rejected = !domain_error;
        }
        return rejected;
    };
    require(rejects_conversion(std::numeric_limits<double>::quiet_NaN(), true),
            "numeric conversion accepted NaN as an i64");
    require(rejects_conversion(std::numeric_limits<double>::infinity(), false) &&
                rejects_conversion(-std::numeric_limits<double>::infinity(), false) &&
                rejects_conversion(std::nextafter(-std::ldexp(1.0, 63), -std::numeric_limits<double>::infinity()),
                                   false) &&
                rejects_conversion(std::ldexp(1.0, 63), false),
            "numeric conversion accepted a value outside the i64 range");

    for (const auto zero : {0.0, -0.0}) {
        const auto division_graph = [zero](wp<TSF<NumericFrame>> source) {
            return bind_expression(source, col("price") / zero);
        };
        auto rejected = false;
        try {
            static_cast<void>(testing::test_graph(
                division_graph,
                testing::input(TSF<NumericFrame>::type(),
                               {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"price", 1.0}}}})));
        } catch (const std::domain_error &) {
            rejected = true;
        }
        require(rejected, "division by positive or negative zero did not report a domain error");
    }
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

    auto foreign_wiring = GraphWiring{};
    auto foreign_point = WiringPoint{};
    {
        auto foreign_context = WiringContext{foreign_wiring};
        foreign_point = foreign_wiring.borrowed_input(TSC<double>::type());
    }
    auto rejected_foreign_point = false;
    try {
        static_cast<void>(bind_expression(col("value"), "value"_n = foreign_point));
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

void test_scalar_capture_and_named_string_parameter() {
    const auto expression = [] {
        const auto source = ScalarValue<string>{std::string{"captured string"}};
        return capture(source) + param("suffix");
    }();
    const auto graph = [&](wp<TS<string>> suffix) {
        return bind_expression(expression, "suffix"_n = suffix);
    };
    const auto values = std::array<std::string_view, 2>{" first", " second"};
    const auto actual = testing::test_graph(graph, values);
    const auto next_values = std::array<std::string_view, 2>{" third", " fourth"};
    const auto next_actual = testing::test_graph(graph, next_values);
    require(actual.equals<string>({"captured string first", "captured string second"}) &&
                next_actual.equals<string>({"captured string third", "captured string fourth"}),
            "a scalar string capture or named string parameter did not survive expression binding");
}

} // namespace

int main() {
    try {
        test_packed_boolean_expression_result();
        test_column_block_function_provider();
        test_erased_unary_and_mixed_column_callbacks();
        test_column_expression_builder_integration();
        test_symbol_producing_column_block_function();
        test_deferred_column_union_matches_expression_form();
        test_column_expression_requires_a_column_operand();
        test_binary_expression_bundle_schema();
        test_static_frame_expression_binding();
        test_numeric_comparison_expression_wiring();
        test_filter_expression_with_multiple_parameters();
        test_mixed_expression_chaining_and_filter();
        test_mixed_series_bindings_keep_independent_types();
        test_mixed_operator_requirements();
        test_mixed_expression_parameter_invalidation();
        test_comparison_runtime<i64, std::equal_to<>>(operators::eq);
        test_comparison_runtime<i64, std::not_equal_to<>>(operators::ne);
        test_comparison_runtime<i64, std::less<>>(operators::lt);
        test_comparison_runtime<i64, std::less_equal<>>(operators::le);
        test_comparison_runtime<i64, std::greater<>>(operators::gt);
        test_comparison_runtime<i64, std::greater_equal<>>(operators::ge);
        test_comparison_runtime<double, std::equal_to<>>(operators::eq);
        test_comparison_runtime<double, std::not_equal_to<>>(operators::ne);
        test_comparison_runtime<double, std::less<>>(operators::lt);
        test_comparison_runtime<double, std::less_equal<>>(operators::le);
        test_comparison_runtime<double, std::greater<>>(operators::gt);
        test_comparison_runtime<double, std::greater_equal<>>(operators::ge);
        test_text_and_symbol_comparison_filters();
        test_comparison_expression_with_tick_parameter();
        test_comparison_uses_forced_concrete_intermediate();
        test_numeric_arithmetic_and_conversion_updates();
        test_numeric_null_and_range_errors();
        test_runtime_schema_expression_binding();
        test_expression_bindings_are_per_call_and_owned();
        test_expression_binding_errors_name_the_binding();
        test_scalar_capture_and_named_string_parameter();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
