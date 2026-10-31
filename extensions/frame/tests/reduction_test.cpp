#include <catch2/catch_test_macros.hpp>

#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/library/operators.hpp>
#include <bg/run_graph.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

struct IntegerSourceState {
    row_id_t first{no_row};
    row_id_t second{no_row};
    row_id_t third{no_row};
    row_id_t fourth{no_row};
};

struct IntegerSource {
    In<TS<i64>> tick;
    State<IntegerSourceState> state;
    Out<TSC<i64>> output;

    void evaluate() const {
        switch (*tick) {
        case 0:
            state->first = output.add(10);
            state->second = output.add(std::nullopt);
            state->third = output.add(-4);
            return;
        case 1:
            output.set(state->first, 7);
            output.set(state->second, 5);
            output.set(state->third, std::nullopt);
            state->fourth = output.add(2);
            return;
        case 2:
            CHECK(output.erase(state->first));
            CHECK(output.erase(state->fourth));
            output.set(state->second, std::nullopt);
            output.add(std::nullopt);
            return;
        case 3:
            output.invalidate();
            return;
        case 4:
            output.clear();
            output.add(9);
            return;
        default:
            return;
        }
    }
};

TEST_CASE("TSC integer sum applies row deltas and invalidation",
          "[feature:operator][series:value][scalar:i64][shape:chain][transition:update][transition:invalidation]") {
    const auto source = node<IntegerSource>();
    const auto graph = [&](wp<TS<i64>> tick) -> wp<TS<i64>> {
        return operators::sum_(source(tick)).as<TS<i64>>();
    };

    const auto result = run_graph(graph, std::array<i64, 6>{0, 1, 2, 3, 4, 5});
    CHECK(result.equals<i64>({6, 14, 0, std::nullopt, 9, std::nullopt}));
}

struct IntegerWrapSourceState {
    row_id_t maximum{no_row};
};

struct IntegerWrapSource {
    In<TS<i64>> tick;
    State<IntegerWrapSourceState> state;
    Out<TSC<i64>> output;

    void evaluate() const {
        if (*tick == 0) {
            state->maximum = output.add(std::numeric_limits<i64>::max());
            output.add(i64{1});
        } else if (*tick == 1) {
            CHECK(output.erase(state->maximum));
        }
    }
};

TEST_CASE("TSC integer sum wraps at the scalar width",
          "[feature:operator][series:value][scalar:i64][shape:chain][transition:removal]") {
    const auto source = node<IntegerWrapSource>();
    const auto graph = [&](wp<TS<i64>> tick) -> wp<TS<i64>> {
        return operators::sum_(source(tick)).as<TS<i64>>();
    };

    CHECK(run_graph(graph, std::array<i64, 2>{0, 1})
              .equals<i64>({std::numeric_limits<i64>::min(), 1}));
}

struct FloatingSourceState {
    row_id_t first{no_row};
    row_id_t second{no_row};
    row_id_t third{no_row};
};

struct FloatingSource {
    In<TS<i64>> tick;
    State<FloatingSourceState> state;
    Out<TSC<double>> output;

    void evaluate() const {
        switch (*tick) {
        case 0:
            state->first = output.add(std::numeric_limits<double>::max());
            state->second = output.add(std::numeric_limits<double>::max());
            state->third = output.add(-std::numeric_limits<double>::max());
            return;
        case 1:
            CHECK(output.erase(state->second));
            return;
        case 2:
            CHECK(output.erase(state->third));
            return;
        case 3:
            output.set(state->first, std::numeric_limits<double>::infinity());
            return;
        case 4:
            state->second = output.add(-std::numeric_limits<double>::infinity());
            return;
        case 5:
            CHECK(output.erase(state->second));
            return;
        case 6:
            output.set(state->first, std::numeric_limits<double>::quiet_NaN());
            return;
        case 7:
            CHECK(output.erase(state->first));
            return;
        case 8:
            state->first = output.add(2.0);
            return;
        default:
            return;
        }
    }
};

TEST_CASE("TSC double sum recovers after overflow and non-finite changes",
          "[feature:operator][series:value][scalar:floating-point][shape:chain][transition:update][transition:removal]") {
    const auto source = node<FloatingSource>();
    const auto graph = [&](wp<TS<i64>> tick) -> wp<TS<double>> {
        return operators::sum_(source(tick)).as<TS<double>>();
    };

    const auto result = run_graph(graph, std::array<i64, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8});
    const auto positive_infinity = std::numeric_limits<double>::infinity();
    CHECK((result[0] && result[0]->view().value() == positive_infinity));
    CHECK((result[1] && result[1]->view().value() == 0.0));
    CHECK((result[2] && result[2]->view().value() == std::numeric_limits<double>::max()));
    CHECK((result[3] && result[3]->view().value() == positive_infinity));
    CHECK((result[4] && std::isnan(result[4]->view().value())));
    CHECK((result[5] && result[5]->view().value() == positive_infinity));
    CHECK((result[6] && std::isnan(result[6]->view().value())));
    CHECK((result[7] && result[7]->view().value() == 0.0));
    CHECK((result[8] && result[8]->view().value() == 2.0));
}

struct RecyclingSourceState {
    row_id_t row{no_row};
};

struct RecyclingSource {
    In<TS<i64>> tick;
    State<RecyclingSourceState> state;
    Out<TSC<i64>> output;

    void evaluate() const {
        switch (*tick) {
        case 0:
            state->row = output.add(4);
            return;
        case 1:
            CHECK(output.erase(state->row));
            return;
        case 2: {
            const auto recycled = output.add(9);
            CHECK(recycled == state->row);
            state->row = recycled;
            return;
        }
        default:
            return;
        }
    }
};

TEST_CASE("TSC sum handles row ID reuse after removal",
          "[feature:operator][series:value][scalar:i64][shape:chain][transition:removal]") {
    const auto source = node<RecyclingSource>();
    const auto graph = [&](wp<TS<i64>> tick) -> wp<TS<i64>> {
        return operators::sum_(source(tick)).as<TS<i64>>();
    };

    CHECK(run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({4, 0, 9}));
}

struct SelectColumnSource {
    In<TS<i64>> selection;
    In<TSC<i64>> first;
    In<TSC<i64>> second;
    Out<TSC<i64>, view> output;

    void evaluate() const {
        if (*selection == 0)
            output.bind(first);
        else
            output.bind(second);
    }
};

TEST_CASE("TSC sum rebuilds when its source column binding changes",
          "[feature:operator][series:value][scalar:i64][shape:chain][transition:update]") {
    const auto select = node<SelectColumnSource>();
    const auto graph = [&](wp<TS<i64>> selection, wp<TSC<i64>> first, wp<TSC<i64>> second) -> wp<TS<i64>> {
        return operators::sum_(select(selection, first, second)).as<TS<i64>>();
    };

    const auto result = run_graph(graph, std::array<i64, 3>{0, 0, 1},
                                  input<TSC<i64>>({literal::TimeSeriesLiteral{}, {i64{4}}, literal::SKIP}),
                                  input<TSC<i64>>({literal::TimeSeriesLiteral{}, {i64{7}}, literal::SKIP}));
    CHECK(result.equals<i64>({0, 4, 7}));
}

struct IndependentSumsSourceState {
    row_id_t row{no_row};
};

template <i64 Initial, i64 Updated> struct IndependentSumsSource {
    In<TS<i64>> tick;
    State<IndependentSumsSourceState> state;
    Out<TSC<i64>> output;

    void evaluate() const {
        switch (*tick) {
        case 0:
            state->row = output.add(Initial);
            return;
        case 1:
            output.set(state->row, Updated);
            return;
        case 2:
            CHECK(output.erase(state->row));
            return;
        default:
            return;
        }
    }
};

TEST_CASE("two TSC sums keep independent state",
          "[feature:operator][series:value][scalar:i64][shape:chain][transition:update]") {
    const auto first = node<IndependentSumsSource<3, 5>>();
    const auto second = node<IndependentSumsSource<7, 11>>();
    const auto graph = [&](wp<TS<i64>> tick) -> wp<TS<i64>> {
        return operators::add(operators::sum_(first(tick)), operators::sum_(second(tick))).as<TS<i64>>();
    };

    CHECK(run_graph(graph, std::array<i64, 3>{0, 1, 2}).equals<i64>({10, 16, 0}));
}

template <template <typename, auto...> typename Field> struct FilteredRows {
    Field<i64> id;
    Field<i64> bucket;
    Field<double> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

TEST_CASE("sum over a filtered column tracks membership changes",
          "[feature:operator][series:value][schema:named][scalar:floating-point][shape:chain][transition:update][transition:removal]") {
    const auto graph = [](wp<TSF<FilteredRows>> source) {
        const auto selected = filter(source, col("bucket") == i64{1});
        return operators::sum_(select_column<double>(selected, util::symbol{"value"})).as<TS<double>>();
    };

    const auto result = run_graph(
        graph,
        input(TSF<FilteredRows>::type(),
              {literal::TimeSeriesLiteral{},
               {{{"id", i64{10}}, {"bucket", i64{1}}, {"value", 10.0}},
                {{"id", i64{70}}, {"bucket", i64{0}}, {"value", 100.0}},
                {{"id", i64{900}}, {"bucket", i64{1}}, {"value", NULL_VALUE}}},
               {{{"id", i64{10}}, {"value", 5.0}},
                {{"id", i64{70}}, {"bucket", i64{1}}},
                {{"id", i64{900}}, {"value", 2.0}}},
               {{{"id", i64{70}}, {"bucket", i64{0}}}, literal::REMOVE({{"id", i64{10}}})},
               {{{"id", i64{30}}, {"bucket", i64{1}}, {"value", 3.0}}},
               {{{"id", i64{70}}, {"bucket", i64{1}}}}}));

    CHECK(result.equals<double>({0.0, 10.0, 107.0, 2.0, 5.0, 105.0}));
}

} // namespace
