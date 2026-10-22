#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tick_format_operations.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <cstddef>
#include <span>
#include <string>

using namespace bg;

namespace {

template <typename Result> std::string format_tick(const Result &result, std::size_t index) {
    auto text = std::string{};
    result.visit_tick(index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
        text = fmt::format("{}", tick(type, valid, reader));
    });
    return text;
}

struct live_and_retained_tick_formatter {
    In<TS<i64>, optional> input;
    Out<TS<string>> output;

    void evaluate(EvaluationContext *context) const {
        const auto now = context->evaluation_time();
        const auto live = fmt::format("{}", tick(input, now));
        const auto buffer = util::buffer::contiguous(64);
        auto writer = TimeSeriesTickWriter{buffer};
        (void)writer.write(input, now);
        const auto reader = util::buffer_reader{buffer};
        const auto retained = fmt::format("{}", tick(input.runtime_type().type(), reader));
        output.set(fmt::format("{}|{}", live, retained));
    }
};

class CustomTimeSeriesType final : public TimeSeriesType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "CustomTimeSeries"; }
};

const CustomTimeSeriesType &custom_time_series_type() {
    static auto type = util::static_storage<CustomTimeSeriesType>{};
    return type.get();
}

void format_custom_time_series(const TimeSeriesType &, util::buffer_reader &reader, fmt::format_context &context) {
    context.advance_to(fmt::format_to(context.out(), "custom({})", reader.read_raw<i64>()));
}

} // namespace

TEST_CASE("live and retained scalar ticks format the same value",
          "[feature:test-graph][series:value][scalar:i64][shape:single-node]") {
    const auto formatter = node(live_and_retained_tick_formatter{});
    const auto graph = [&](wp<TS<i64>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {42, literal::INVALID});

    CHECK(result.equals<string>({"42|42", "INVALID|INVALID"}));
}

TEST_CASE("retained dictionary tick formatting preserves deltas and nested invalidity",
          "[feature:test-graph][series:tsd][scalar:i64][transition:update][transition:removal]") {
    const auto identity = [](wp<TSD<i64, TS<i64>>> input) { return input; };
    const auto result = testing::test_graph(identity, {{{1, 10}}, {{1, 11}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}});

    CHECK(format_tick(result, 0) == "{1: 10}");
    CHECK(format_tick(result, 1) == "{1: 11}");
    CHECK(format_tick(result, 2) == "{1: INVALID}");
    CHECK(format_tick(result, 3) == "{1: REMOVE}");
}

TEST_CASE("retained nested dictionary ticks format child payloads recursively",
          "[feature:test-graph][series:tsd][scalar:i64][shape:doubly-nested]") {
    const auto identity = [](wp<TSD<i64, TSD<i64, TS<i64>>>> input) { return input; };
    const auto result = testing::test_graph(identity, {{{1, {{2, 10}}}}});

    CHECK(format_tick(result, 0) == "{1: {2: 10}}");
}

TEST_CASE("retained tick formatting leaves captured payload readers unchanged",
          "[feature:test-graph][series:value][scalar:string]") {
    const auto identity = [](wp<TS<string>> input) { return input; };
    const auto result = testing::test_graph(identity, {"first"});

    REQUIRE(format_tick(result, 0) == "\"first\"");
    CHECK(format_tick(result, 0) == "\"first\"");
}

TEST_CASE("retained tick formatting dispatches extension time-series types",
          "[feature:visitor][series:generic][declaration:runtime]") {
    const auto added = TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance().add(
        custom_time_series_type(), TimeSeriesTickFormatOperations{.serialized = &format_custom_time_series});
    REQUIRE(added);

    auto buffer = util::buffer::contiguous(64);
    auto writer = util::buffer_writer{buffer};
    auto context = ScalarBufferWriteContext{writer};
    details::TickHeader{details::TickOperation::tick, true}.write(context);
    const auto payload = i64{42};
    context.write_bytes(std::as_bytes(std::span{&payload, std::size_t{1}}));
    writer.commit();
    const auto reader = util::buffer_reader{buffer};

    CHECK(fmt::format("{}", tick(custom_time_series_type(), reader)) == "custom(42)");
}
