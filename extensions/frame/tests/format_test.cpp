#include <bg/frame/frame.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/format.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tick_serializer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct FormatRows {
    Field<i64> id;
    Field<i64> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <typename Series> struct live_and_retained_formatter {
    In<Series> input;
    Out<TS<string>> output;

    void evaluate(EvaluationContext *context) const {
        const auto time = context->evaluation_time();
        const auto live = fmt::format("{}", tick(input, time));
        const auto buffer = util::buffer::contiguous(4096);
        auto writer = TimeSeriesTickWriter{buffer};
        if (!writer.write(input, time))
            throw std::runtime_error{"could not retain TSF tick for formatting"};
        auto cleanup = TimeSeriesTickReader{buffer, input.runtime_type().type()};
        const auto reader = util::buffer_reader{buffer};
        const auto retained = fmt::format("{}", tick(input.runtime_type().type(), reader));
        output.set(fmt::format("{}|{}", live, retained));
    }
};

template <typename Cell> struct triggered_column_formatter {
    In<TSC<Cell>, optional> input;
    In<TS<i64>> trigger;
    Out<TS<string>> output;

    void evaluate(EvaluationContext *context) const {
        const auto time = context->evaluation_time();
        const auto live = fmt::format("{}", tick(input, time));
        const auto stale = fmt::format("{}", tick(input, time + std::chrono::microseconds{1}));
        const auto buffer = util::buffer::contiguous(4096);
        auto writer = TimeSeriesTickWriter{buffer};
        if (!writer.write(input, time)) {
            output.set(fmt::format("{}|{}|SKIP", live, stale));
            return;
        }
        auto cleanup = TimeSeriesTickReader{buffer, input.runtime_type().type()};
        const auto reader = util::buffer_reader{buffer};
        const auto retained = fmt::format("{}", tick(input.runtime_type().type(), reader));
        const auto repeated = fmt::format("{}", tick(input.runtime_type().type(), reader));
        output.set(fmt::format("{}|{}|{}|{}", live, stale, retained, repeated));
    }
};

} // namespace

TEST_CASE("native Frame scalar formatting traverses column batches", "[feature:frame][scalar:frame]") {
    const auto value = ScalarValue<Frame<FormatRows>>::from_literal(
        {{"id", {i64{1}, i64{2}}}, {"value", {i64{10}, NULL_VALUE}}});

    CHECK(fmt::format("{}", value.view()) == "Frame(rows=2, columns={\"id\": [1, 2], \"value\": [10, NULL_VALUE]})");
}

TEST_CASE("Column scalar formatting handles boolean, string, symbol, and null cells",
          "[feature:frame][scalar:column]") {
    const auto booleans = ScalarValue<Column<bool>>::from_literal({true, false});
    const auto strings = ScalarValue<Column<string>>::from_literal({std::string_view{"text"}, NULL_VALUE});
    const auto symbols = ScalarValue<Column<symbol>>::from_literal({symbol{"key"}, NULL_VALUE});

    CHECK(fmt::format("{}", booleans.view()) == "[true, false]");
    CHECK(fmt::format("{}", strings.view()) == "[\"text\", NULL_VALUE]");
    CHECK(fmt::format("{}", symbols.view()) == "[\"key\", NULL_VALUE]");
}

TEST_CASE("live and retained TSC formatting handles strings, symbols, booleans, nulls, and SKIP",
          "[feature:frame][series:tsc]") {
    const auto string_formatter = node<triggered_column_formatter<string>>();
    const auto string_graph = [&](wp<TSC<string>> values, wp<TS<i64>> trigger) {
        return string_formatter(values, trigger);
    };
    const auto string_values = input<TSC<string>>({literal::TimeSeriesLiteral{},
                                                   {std::string_view{"text"}, NULL_VALUE}, literal::SKIP,
                                                   literal::INVALID, literal::TimeSeriesLiteral{}});
    const auto triggers = std::array<i64, 5>{0, 1, 2, 3, 4};
    const auto strings = run_graph(string_graph, string_values, triggers);
    REQUIRE(strings.size() == 5);
    CHECK(strings[0]->view().value() == "{}|SKIP|{}|{}");
    CHECK(strings[1]->view().value() ==
          "{0: \"text\", 1: NULL_VALUE}|SKIP|{0: \"text\", 1: NULL_VALUE}|{0: \"text\", 1: NULL_VALUE}");
    CHECK(strings[2]->view().value() == "SKIP|SKIP|SKIP");
    CHECK(strings[3]->view().value() == "INVALID|SKIP|INVALID|INVALID");
    CHECK(strings[4]->view().value() == "{}|SKIP|{}|{}");

    const auto symbol_formatter = node<triggered_column_formatter<symbol>>();
    const auto symbol_graph = [&](wp<TSC<symbol>> values, wp<TS<i64>> trigger) {
        return symbol_formatter(values, trigger);
    };
    const auto symbol_values = input<TSC<symbol>>(
        {literal::TimeSeriesLiteral{}, {symbol{"venue"}, NULL_VALUE}, literal::SKIP});
    const auto short_triggers = std::array<i64, 3>{0, 1, 2};
    const auto symbols = run_graph(symbol_graph, symbol_values, short_triggers);
    REQUIRE(symbols.size() == 3);
    CHECK(symbols[1]->view().value() ==
          "{0: \"venue\", 1: NULL_VALUE}|SKIP|{0: \"venue\", 1: NULL_VALUE}|{0: \"venue\", 1: NULL_VALUE}");
    CHECK(symbols[2]->view().value() == "SKIP|SKIP|SKIP");

    const auto boolean_formatter = node<triggered_column_formatter<bool>>();
    const auto boolean_graph = [&](wp<TSC<bool>> values, wp<TS<i64>> trigger) {
        return boolean_formatter(values, trigger);
    };
    const auto boolean_values = input<TSC<bool>>({literal::TimeSeriesLiteral{}, {true, false}, literal::SKIP});
    const auto booleans = run_graph(boolean_graph, boolean_values, short_triggers);
    REQUIRE(booleans.size() == 3);
    CHECK(booleans[1]->view().value() == "{0: true, 1: false}|SKIP|{0: true, 1: false}|{0: true, 1: false}");
    CHECK(booleans[2]->view().value() == "SKIP|SKIP|SKIP");
}

TEST_CASE("live and retained TSF ticks format the same row and field deltas", "[feature:frame][series:tsf]") {
    const auto formatter = node<live_and_retained_formatter<TSF<FormatRows>>>();
    const auto graph = [&](wp<TSF<FormatRows>> rows) { return formatter(rows); };
    const auto updates = input<TSF<FormatRows>>(
        {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"value", i64{10}}}},
         {{{"id", i64{1}}, {"value", i64{11}}}}, {literal::REMOVE({{"id", i64{1}}})}});
    const auto result = run_graph(graph, updates);

    REQUIRE(result.size() == 4);
    CHECK(result[0]->view().value() == "{}|{}");
    CHECK(result[1]->view().value() ==
          "{0: {\"id\": 1, \"value\": 10}}|{0: {\"id\": 1, \"value\": 10}}");
    CHECK(result[2]->view().value() ==
          "{0: {\"id\": 1, \"value\": 11}}|{0: {\"id\": 1, \"value\": 11}}");
    CHECK(result[3]->view().value() == "{0: REMOVE}|{0: REMOVE}");
}
