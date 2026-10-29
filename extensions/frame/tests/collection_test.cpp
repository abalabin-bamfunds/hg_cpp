#include <catch2/catch_test_macros.hpp>

#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/library/operators.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>

#include <array>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct KeyedValues {
    Field<i64> id;
    Field<i64> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

TEST_CASE("len_ and is_empty query native TSC membership", "[feature:frame][feature:operator][series:tsc]") {
    const auto updates = input<TSC<i64>>(
        {literal::TimeSeriesLiteral{}, {i64{10}, NULL_VALUE}, literal::INVALID, literal::TimeSeriesLiteral{},
         {i64{20}}});
    const auto length = [](wp<TSC<i64>> value) { return operators::len_(value).as<TS<i64>>(); };
    const auto empty = [](wp<TSC<i64>> value) { return operators::is_empty(value).as<TS<bool>>(); };
    const auto validity = [](wp<TSC<i64>> value) { return operators::valid(value).as<TS<bool>>(); };
    const auto all_validity = [](wp<TSC<i64>> value) { return operators::all_valid(value).as<TS<bool>>(); };
    const auto count = [](wp<TSC<i64>> value) { return operators::count(value).as<TS<i64>>(); };

    CHECK(run_graph(length, updates).equals<i64>({0, 2, std::nullopt, 0, 1}));
    CHECK(run_graph(empty, updates).equals<bool>({true, false, std::nullopt, true, false}));
    CHECK(run_graph(validity, updates).equals<bool>({true, std::nullopt, false, true, std::nullopt}));
    CHECK(run_graph(all_validity, updates).equals<bool>({true, std::nullopt, false, true, std::nullopt}));
    CHECK(run_graph(count, updates).equals<i64>({1, 2, std::nullopt, 1, 2}));
}

TEST_CASE("len_ and is_empty query declared TSF membership",
          "[feature:frame][feature:operator][series:tsf][transition:addition][transition:update]"
          "[transition:removal][transition:invalidation][transition:revalidation]") {
    const auto updates = input<TSF<KeyedValues>>(
        {{{{"id", i64{1}}, {"value", i64{10}}}, {{"id", i64{2}}, {"value", NULL_VALUE}}},
         {{{"id", i64{1}}, {"value", i64{11}}}},
         {literal::REMOVE({{"id", i64{2}}})},
         literal::INVALID,
         literal::TimeSeriesLiteral{},
         {{{"id", i64{3}}, {"value", NULL_VALUE}}}});
    const auto length = [](wp<TSF<KeyedValues>> value) { return operators::len_(value).as<TS<i64>>(); };
    const auto empty = [](wp<TSF<KeyedValues>> value) { return operators::is_empty(value).as<TS<bool>>(); };
    const auto validity = [](wp<TSF<KeyedValues>> value) { return operators::valid(value).as<TS<bool>>(); };
    const auto all_validity = [](wp<TSF<KeyedValues>> value) {
        return operators::all_valid(value).as<TS<bool>>();
    };
    const auto count = [](wp<TSF<KeyedValues>> value) { return operators::count(value).as<TS<i64>>(); };

    CHECK(run_graph(length, updates).equals<i64>({2, 2, 1, std::nullopt, 0, 1}));
    CHECK(run_graph(empty, updates).equals<bool>({false, false, false, std::nullopt, true, false}));
    CHECK(run_graph(validity, updates).equals<bool>(
        {true, std::nullopt, std::nullopt, false, true, std::nullopt}));
    CHECK(run_graph(all_validity, updates).equals<bool>(
        {true, std::nullopt, std::nullopt, false, true, std::nullopt}));
    CHECK(run_graph(count, updates).equals<i64>({1, 2, 3, std::nullopt, 1, 2}));
}

TEST_CASE("len_ queries runtime-schema projections, filters, and computed frames",
          "[feature:frame][feature:operator][series:tsf][schema:unnamed][declaration:runtime]") {
    const auto fields = std::array{SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
                                   SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>()}};
    const auto &schema = anonymous_schema(fields, key_metadata<"id">());
    const auto &runtime_schema = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    const auto updates = input(
        runtime_schema,
        {literal::TimeSeriesLiteral{},
         {{{"id", i64{1}}, {"value", i64{10}}}, {{"id", i64{2}}, {"value", NULL_VALUE}}},
         {{{"id", i64{1}}, {"value", i64{11}}}},
         {{{"id", i64{2}}, {"value", i64{12}}}},
         {literal::REMOVE({{"id", i64{2}}})}});
    const auto selected = [](wp<TSF<SCHEMA>> value) {
        return operators::len_(frame::select(value, "value")).as<TS<i64>>();
    };
    const auto filtered = [](wp<TSF<SCHEMA>> value) {
        return operators::len_(frame::filter(value, col("id") > i64{1})).as<TS<i64>>();
    };
    const auto computed = [](wp<TSF<SCHEMA>> value) {
        return operators::len_(frame::with_columns(value, "twice"_n = col("value") * i64{2})).as<TS<i64>>();
    };
    const auto computed_column = [](wp<TSF<SCHEMA>> value) {
        return operators::len_(frame::with_columns(value, "twice"_n = col("value") * i64{2})["twice"])
            .as<TS<i64>>();
    };
    const auto filtered_column = [](wp<TSF<SCHEMA>> value) {
        return operators::is_empty(frame::filter(value, col("id") > i64{1})["value"]).as<TS<bool>>();
    };

    CHECK(run_graph(selected, updates).equals<i64>({0, 2, 2, 2, 1}));
    CHECK(run_graph(filtered, updates).equals<i64>({0, 1, std::nullopt, 1, 0}));
    CHECK(run_graph(computed, updates).equals<i64>({0, 2, 2, 2, 1}));
    CHECK(run_graph(computed_column, updates).equals<i64>({0, 2, 2, 2, 1}));
    CHECK(run_graph(filtered_column, updates).equals<bool>({true, false, std::nullopt, false, true}));
}

template <template <typename, auto...> typename Field> struct EmptyFrame {};

struct EmptyFrameState {
    row_id_t first{no_row};
};

struct EmptyFrameSource {
    In<TS<i64>> command;
    State<EmptyFrameState> state;
    Out<TSF<EmptyFrame>> output;

    void evaluate() const {
        switch (*command) {
        case 1:
            state->first = output.add(EmptyFrame<Cell>{});
            return;
        case 2:
            static_cast<void>(output.add(EmptyFrame<Cell>{}));
            return;
        case 3:
            CHECK(output.erase(state->first));
            return;
        case 4:
            output.clear();
            return;
        default:
            return;
        }
    }
};

TEST_CASE("len_ and is_empty count rows in zero-field TSF", "[feature:frame][feature:operator][series:tsf]") {
    const auto source = node<EmptyFrameSource>();
    const auto length = [&](wp<TS<i64>> command) { return operators::len_(source(command)).as<TS<i64>>(); };
    const auto empty = [&](wp<TS<i64>> command) { return operators::is_empty(source(command)).as<TS<bool>>(); };
    constexpr auto commands = std::array<i64, 4>{1, 2, 3, 4};

    CHECK(run_graph(length, commands).equals<i64>({1, 2, 1, 0}));
    CHECK(run_graph(empty, commands).equals<bool>({false, false, false, true}));
}

} // namespace
