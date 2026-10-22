#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tick_format_operations.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
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

template <typename SERIES> struct live_and_retained_tick_formatter {
    In<SERIES, optional> input;
    Out<TS<string>> output;

    void evaluate(EvaluationContext *context) const {
        const auto now = context->evaluation_time();
        auto live = std::string{};
        fmt::format_to(std::back_inserter(live), "[{}]", tick(input, now));
        const auto buffer = util::buffer::contiguous(4096);
        auto writer = TimeSeriesTickWriter{buffer};
        if (!writer.write(input, now)) {
            output.set(fmt::format("{}|[SKIP]", live));
            return;
        }
        auto cleanup = TimeSeriesTickReader{buffer, input.runtime_type().type()};
        const auto reader = util::buffer_reader{buffer};
        auto retained = std::string{};
        fmt::format_to(std::back_inserter(retained), "[{}]", tick(input.runtime_type().type(), reader));
        output.set(fmt::format("{}|{}", live, retained));
    }
};

struct stale_tick_formatter {
    In<TS<i64>, optional> input;
    Out<TS<string>> output;

    void evaluate(EvaluationContext *context) const {
        auto text = std::string{};
        fmt::format_to(std::back_inserter(text), "<{}>",
                       tick(input, context->evaluation_time() + std::chrono::microseconds{1}));
        output.set(std::move(text));
    }
};

template <template <typename, auto...> typename Field> struct TickFormatBundle {
    Field<TS<i64>> count;
    Field<TS<string>> name;
};

template <template <typename, auto...> typename Field> struct NestedTickFormatBundle {
    Field<TSD<i64, TS<i64>>> values;
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

class CustomTimeSeriesRuntimeType final : public TimeSeriesRuntimeType {
  public:
    explicit CustomTimeSeriesRuntimeType(const TimeSeriesType &type)
        : TimeSeriesRuntimeType{type, sizeof(i64), alignof(i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<i64 *>(destination));
    }

    void destroy_value(void *value, allocator &) const noexcept override { std::destroy_at(static_cast<i64 *>(value)); }
};

void format_custom_live_time_series(const In<TS_T> &input, EvaluationTime, fmt::format_context &context) {
    context.advance_to(fmt::format_to(context.out(), "semantic({})", *static_cast<const i64 *>(input.data())));
}

void format_custom_live_runtime(const In<TS_T> &input, EvaluationTime, fmt::format_context &context) {
    context.advance_to(fmt::format_to(context.out(), "runtime({})", *static_cast<const i64 *>(input.data())));
}

} // namespace

TEST_CASE("live and retained scalar ticks format the same value",
          "[feature:test-graph][series:value][scalar:i64][shape:single-node]") {
    const auto formatter = node<live_and_retained_tick_formatter<TS<i64>>>();
    const auto graph = [&](wp<TS<i64>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {42, literal::INVALID});

    CHECK(result.equals<string>({"[42]|[42]", "[INVALID]|[INVALID]"}));
}

TEST_CASE("live and retained byte ticks format the same payload",
          "[feature:test-graph][series:value][scalar:bytes]") {
    const auto formatter = node<live_and_retained_tick_formatter<TS<bytes>>>();
    const auto graph = [&](wp<TS<bytes>> input) { return formatter(input); };
    const auto payload = std::array{byte{0x00}, byte{0x7f}, byte{0xff}};
    const auto result = testing::test_graph(
        graph, testing::input<TS<bytes>>({std::span<const byte>{payload}}));

    CHECK(result.equals<string>({"[bytes(\"007fff\")]|[bytes(\"007fff\")]"}));
}

TEST_CASE("live tick formatting writes SKIP for an unchanged root input",
          "[feature:test-graph][series:value][scalar:i64]") {
    const auto formatter = node<stale_tick_formatter>();
    const auto graph = [&](wp<TS<i64>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {42});

    CHECK(result.equals<string>({"<SKIP>"}));
}

TEST_CASE("retained dictionary tick formatting preserves deltas and nested invalidity",
          "[feature:test-graph][series:tsd][scalar:i64][transition:update][transition:removal]") {
    const auto identity = [](wp<TSD<i64, TS<i64>>> input) { return input; };
    const auto result = testing::test_graph(
        identity, {{{1, 10}}, {{1, 11}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}});

    CHECK(format_tick(result, 0) == "{1: 10}");
    CHECK(format_tick(result, 1) == "{1: 11}");
    CHECK(format_tick(result, 2) == "{1: INVALID}");
    CHECK(format_tick(result, 3) == "{1: REMOVE}");
}

TEST_CASE("live and retained fixed-list ticks format the same changed items",
          "[feature:test-graph][series:fixed-tsl][scalar:i64][transition:update]") {
    const auto formatter = node<live_and_retained_tick_formatter<TSL<TS<i64>, 3>>>();
    const auto graph = [&](wp<TSL<TS<i64>, 3>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {{{0, 10}, {2, 30}}});

    CHECK(result.equals<string>({"[{0: 10, 2: 30}]|[{0: 10, 2: 30}]"}));
}

TEST_CASE("live and retained set ticks format additions and removals",
          "[feature:test-graph][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto formatter = node<live_and_retained_tick_formatter<TSS<i64>>>();
    const auto graph = [&](wp<TSS<i64>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {{1, 2}, {literal::REMOVED(1)}});

    CHECK(result.equals<string>({"[{1, 2}]|[{1, 2}]", "[{REMOVED(1)}]|[{REMOVED(1)}]"}));
}

TEST_CASE("live and retained bundle ticks format named fields",
          "[feature:test-graph][series:tsb][scalar:i64][scalar:string]") {
    const auto formatter = node<live_and_retained_tick_formatter<TSB<TickFormatBundle>>>();
    const auto graph = [&](wp<TSB<TickFormatBundle>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {{{"count", 3}, {"name", "last"}}});

    CHECK(result.equals<string>({"[{\"count\": 3, \"name\": \"last\"}]|"
                                 "[{\"count\": 3, \"name\": \"last\"}]"}));
}

TEST_CASE("nested dictionary bundle ticks format child collections recursively",
          "[feature:test-graph][series:tsd][series:tsb][shape:deeply-nested]") {
    const auto formatter = node<live_and_retained_tick_formatter<TSD<i64, TSB<NestedTickFormatBundle>>>>();
    const auto graph = [&](wp<TSD<i64, TSB<NestedTickFormatBundle>>> input) { return formatter(input); };
    const auto result = testing::test_graph(graph, {{{1, {{"values", {{2, 20}, {3, 30}}}}}}});

    CHECK(result.equals<string>({"[{1: {\"values\": {2: 20, 3: 30}}}]|"
                                 "[{1: {\"values\": {2: 20, 3: 30}}}]"}));
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
    const auto value = std::string(256, 'x');
    const auto expected = fmt::format("\"{}\"", value);
    const auto result = testing::test_graph(identity, {value});

    REQUIRE(format_tick(result, 0) == expected);
    CHECK(format_tick(result, 0) == expected);
    CHECK(result.equals<string>({value}));
}

TEST_CASE("retained tick formatting dispatches extension time-series types",
          "[feature:visitor][series:generic][declaration:runtime]") {
    auto &dispatch = TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance();
    const auto added = dispatch.add(custom_time_series_type(),
                                    TimeSeriesTickFormatOperations{.live = &format_custom_live_time_series,
                                                                  .serialized = &format_custom_time_series});
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

    static auto semantic_runtime = CustomTimeSeriesRuntimeType{custom_time_series_type()};
    static auto exact_runtime = CustomTimeSeriesRuntimeType{custom_time_series_type()};
    REQUIRE(dispatch.add(semantic_runtime, TimeSeriesTickFormatOperations{}));
    REQUIRE(dispatch.add(exact_runtime, TimeSeriesTickFormatOperations{.live = &format_custom_live_runtime}));

    const auto now = MIN_DT + std::chrono::microseconds{1};
    const auto state = TimeSeriesState{now};
    const auto value = i64{42};
    const auto semantic_input = In<TS_T>{TimeSeriesInputData{semantic_runtime, state, &value, true}};
    const auto exact_input = In<TS_T>{TimeSeriesInputData{exact_runtime, state, &value, true}};

    CHECK(fmt::format("{}", tick(semantic_input, now)) == "semantic(42)");
    CHECK(fmt::format("{}", tick(exact_input, now)) == "runtime(42)");
}
