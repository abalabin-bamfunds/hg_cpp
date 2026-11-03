#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <hg/frame/frame.hpp>
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace {

using namespace hg;
using namespace hg::frame;
using namespace hg::literals;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<symbol> venue;
    Field<i64> size;
};

struct QuoteSource {
    In<TS<i64>> tick;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        if (*tick == 0)
            output.clear();
        if (*tick == 1)
            output.add(Row<Quotes>{.venue = symbol{"X"}, .size = 5});
    }
};

TEST_CASE("group_by builds aggregation wiring through its method and operator APIs",
          "[feature:operator][schema:unnamed][declaration:runtime]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto empty_source = node([](Out<TSF<Quotes>>) {});
    const auto quotes = empty_source();
    const auto grouped = frame::group_by(quotes, "venue");
    const auto sum = operators::sum_(col("size"));
    const auto result = grouped.agg("total"_n = sum);
    const auto &type = static_cast<const FrameSeriesType &>(result.type());
    const auto &schema = static_cast<const Schema &>(type.schema_type());
    REQUIRE(schema.field_count() == 2);
    CHECK(schema.field(0).name == symbol{"venue"});
    CHECK(schema.field(0).type == &cpp_scalar_type<symbol>());
    CHECK(schema.field(1).name == symbol{"total"});
    CHECK(schema.field(1).type == &cpp_scalar_type<i64>());
    const auto *key = schema.metadata().find("hg.frame.key");
    REQUIRE(key != nullptr);
    const auto names = Scalar<List<string>>::from(key->runtime_type(), key->data());
    REQUIRE(names.size() == 1);
    CHECK(names[0].value() == "venue");
    const auto direct = operators::agg(grouped, "total"_n = sum);
    CHECK(&direct.type() == &result.type());
}

TEST_CASE("group_by runs aggregation wiring", "[feature:operator]") {
    const auto source = node<QuoteSource>();
    const auto observe = node([](In<TSF<RESULT>> method_result, In<TSF<RESULT>> operator_result,
                                 In<TS<i64>> tick, Out<TS<i64>> output) {
        static_cast<void>(method_result);
        static_cast<void>(operator_result);
        output.set(*tick);
    });

    const auto graph = [&](wp<TS<i64>> tick) {
        const auto quotes = source(tick);
        const auto grouped = frame::group_by(quotes, "venue");
        const auto sum = operators::sum_(col("size"));
        const auto method_result = grouped.agg("total"_n = sum);
        const auto operator_result = operators::agg(grouped, "total"_n = sum).as<TSF<RESULT>>();
        return observe(method_result, operator_result, tick);
    };

    CHECK(run_graph(graph, std::array<i64, 2>{0, 1}).equals<i64>({0, 1}));
}

template <template <typename, auto...> typename Field> struct Trades {
    Field<i64> id;
    Field<symbol> venue;
    Field<i64> size;
    Field<double> price;
    Field<string> note;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

TEST_CASE("grouped sums apply additions, updates, group changes, and removals",
          "[feature:operator][transition:addition][transition:update][transition:removal]"
          "[schema:unnamed][declaration:runtime]") {
    const auto graph = [](wp<TSF<SCHEMA>> trades) {
        return frame::group_by(trades, "venue").agg("total"_n = operators::sum_(col("size")));
    };
    const auto updates = input<TSF<Trades>>({
        literal::TimeSeriesLiteral{},
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{7}}},
         {{"id", i64{3}}, {"venue", symbol{"Y"}}, {"size", NULL_VALUE}}},
        {{{"id", i64{1}}, {"size", i64{8}}}},
        {{{"id", i64{2}}, {"venue", symbol{"Y"}}}},
        {literal::REMOVE({{"id", i64{1}}})},
        {{{"id", i64{2}}, {"size", NULL_VALUE}}},
        {literal::REMOVE({{"id", i64{2}}})},
        {literal::REMOVE({{"id", i64{3}}})},
        literal::INVALID,
        {{{"id", i64{4}}, {"venue", symbol{"X"}}, {"size", i64{9}}}},
        {{{"id", i64{4}}, {"note", "ignored"}}},
    });
    CHECK(run_graph(graph, updates).equals({
        {},
        {{{"venue", symbol{"X"}}, {"total", i64{12}}}, {{"venue", symbol{"Y"}}, {"total", i64{0}}}},
        {{{"venue", symbol{"X"}}, {"total", i64{15}}}},
        {{{"venue", symbol{"X"}}, {"total", i64{8}}}, {{"venue", symbol{"Y"}}, {"total", i64{7}}}},
        {literal::REMOVE({{"venue", symbol{"X"}}})},
        {{{"venue", symbol{"Y"}}, {"total", i64{0}}}},
        {{{"venue", symbol{"Y"}}, {"total", i64{0}}}},
        {literal::REMOVE({{"venue", symbol{"Y"}}})},
        literal::INVALID,
        {{{"venue", symbol{"X"}}, {"total", i64{9}}}},
        literal::SKIP,
    }));
}

TEST_CASE("grouped sums bind expression operands and captured scalar time series",
          "[feature:operator][schema:unnamed][declaration:runtime][transition:update]") {
    const auto fields = std::array{
        SchemaField{symbol{"id"}, &cpp_scalar_type<i64>()},
        SchemaField{symbol{"venue"}, &cpp_scalar_type<symbol>()},
        SchemaField{symbol{"size"}, &cpp_scalar_type<i64>()},
        SchemaField{symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &schema = anonymous_schema(fields, key_metadata<"id">());
    const auto graph = [](wp<TSF<SCHEMA>> trades, wp<TS<double>> multiplier) {
        return frame::group_by(trades, std::vector<std::string>{"venue"}).agg(
            "total"_n = operators::sum_(col("size") * i64{2}),
            "notional"_n = operators::sum_(col("price") * frame::convert<double>(col("size")) * multiplier));
    };
    const auto updates = input(frame_series_type(schema, TimeSeriesGuarantee::ordinary), {
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}, {"price", 2.0}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{7}}, {"price", 3.0}}},
        literal::SKIP,
        {{{"id", i64{2}}, {"size", i64{10}}}},
        {literal::REMOVE({{"id", i64{1}}})},
    });
    const auto result = run_graph(graph, updates, input<TS<double>>({1.0, 2.0, literal::SKIP, literal::SKIP}));
    INFO(result);
    CHECK(result.equals({
        {{{"venue", symbol{"X"}}, {"total", i64{24}}, {"notional", 31.0}}},
        {{{"venue", symbol{"X"}}, {"notional", 62.0}}},
        {{{"venue", symbol{"X"}}, {"total", i64{30}}, {"notional", 80.0}}},
        {{{"venue", symbol{"X"}}, {"total", i64{20}}, {"notional", 60.0}}},
    }));
}

TEST_CASE("grouped sums initialize from current rows when a captured parameter becomes valid",
          "[feature:operator][feature:invalidation][transition:first-tick][transition:invalidation]"
          "[transition:revalidation][schema:unnamed][declaration:runtime]") {
    const auto graph = [](wp<TSF<SCHEMA>> trades, wp<TS<i64>> multiplier) {
        return frame::group_by(trades, "venue").agg(
            "total"_n = operators::sum_(col("size")),
            "scaled"_n = operators::sum_(col("size") * multiplier));
    };
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}},
         {{"id", i64{2}}, {"venue", symbol{"Y"}}, {"size", i64{7}}}},
        {{{"id", i64{1}}, {"size", i64{8}}},
         {{"id", i64{3}}, {"venue", symbol{"X"}}, {"size", i64{4}}}},
        literal::SKIP,
        {literal::REMOVE({{"id", i64{2}}}), {{"id", i64{1}}, {"size", i64{9}}}},
        {{{"id", i64{1}}, {"venue", symbol{"Y"}}}, {{"id", i64{3}}, {"size", i64{6}}}},
        literal::SKIP,
        {{{"id", i64{3}}, {"size", i64{10}}}},
    });
    const auto multipliers = input<TS<i64>>({
        literal::SKIP, literal::SKIP, i64{2}, literal::INVALID, literal::SKIP, i64{3}, literal::SKIP,
    });
    CHECK(run_graph(graph, updates, multipliers).equals({
        literal::SKIP,
        literal::SKIP,
        {{{"venue", symbol{"X"}}, {"total", i64{12}}, {"scaled", i64{24}}},
         {{"venue", symbol{"Y"}}, {"total", i64{7}}, {"scaled", i64{14}}}},
        literal::INVALID,
        literal::SKIP,
        {{{"venue", symbol{"X"}}, {"total", i64{6}}, {"scaled", i64{18}}},
         {{"venue", symbol{"Y"}}, {"total", i64{9}}, {"scaled", i64{27}}}},
        {{{"venue", symbol{"X"}}, {"total", i64{10}}, {"scaled", i64{30}}}},
    }));
}

TEST_CASE("grouped sums retain composite string keys after source changes",
          "[feature:operator][scalar:string][scalar:symbol][transition:update][transition:removal]") {
    const auto graph = [](wp<TSF<Trades>> trades) {
        return frame::group_by(trades, {"venue", "note"}).agg("total"_n = operators::sum_(col("size")));
    };
    const auto long_key = std::string(96, 'x');
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"note", std::string_view{long_key}}, {"size", i64{4}}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"note", ""}, {"size", i64{5}}}},
        {{{"id", i64{1}}, {"note", ""}}},
        {literal::REMOVE({{"id", i64{1}}})},
        {literal::REMOVE({{"id", i64{2}}})},
    });
    CHECK(run_graph(graph, updates).equals({
        {{{"venue", symbol{"X"}}, {"note", std::string_view{long_key}}, {"total", i64{4}}},
         {{"venue", symbol{"X"}}, {"note", ""}, {"total", i64{5}}}},
        {literal::REMOVE({{"venue", symbol{"X"}}, {"note", std::string_view{long_key}}}),
         {{"venue", symbol{"X"}}, {"note", ""}, {"total", i64{9}}}},
        {{{"venue", symbol{"X"}}, {"note", ""}, {"total", i64{5}}}},
        {literal::REMOVE({{"venue", symbol{"X"}}, {"note", ""}})},
    }));
}

TEST_CASE("grouped sums reject invalid grouping and aggregation declarations during wiring",
          "[feature:operator][feature:wiring]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source = node([](Out<TSF<Trades>>) {});
    const auto trades = source();
    CHECK_THROWS_AS(frame::group_by(trades, "absent"), WiringError);
    CHECK_THROWS_AS(frame::group_by(trades, "venue", "venue"), WiringError);
    CHECK_THROWS_AS(frame::group_by(trades, "price"), WiringError);
    CHECK_THROWS_AS(frame::group_by(trades, std::span<const symbol>{}), WiringError);
    const auto grouped = frame::group_by(trades, "venue");
    CHECK_THROWS_AS(grouped.agg("venue"_n = operators::sum_(col("size"))), WiringError);
    CHECK_THROWS_AS(grouped.agg("total"_n = operators::sum_(col("note"))), WiringError);
    CHECK_THROWS_AS(grouped.agg("total"_n = operators::sum_(col("absent"))), WiringError);
    CHECK_THROWS_AS(grouped.agg("total"_n = col("size")), WiringError);
    CHECK_THROWS_AS(operators::agg[type<TS<i64>>](grouped, "total"_n = operators::sum_(col("size"))), WiringError);
}

TEST_CASE("grouped sums require scalar expression structure during wiring even for const expression time series",
          "[feature:operator][feature:wiring][guarantee:const]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source = node([](Out<TSF<Trades>>) {});
    const auto dynamic_expression = node([](Out<TS<Expression>>) {});
    const auto const_expression = node([](Out<TS<Expression, TimeSeriesGuarantee::const_>>) {});
    const auto factor = node([](Out<TS<double>>) {});
    const auto grouped = frame::group_by(source(), "venue");

    CHECK_THROWS_AS(operators::sum_(dynamic_expression()), WiringError);
    const auto mixed = bundle("expression"_n = const_expression(), "binding_0"_n = factor());
    const auto sum = operators::sum_(mixed);
    CHECK_THROWS_MATCHES(grouped.agg("total"_n = sum), WiringError,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring(
                             "mixed expression bundle has no scalar expression field")));
}

TEST_CASE("grouped sums follow filtered membership and integer wraparound",
          "[feature:operator][scalar:i64][transition:update][transition:removal]") {
    const auto graph = [](wp<TSF<Trades>> trades, wp<TS<i64>> minimum) {
        const auto selected = frame::filter(trades, col("size") > minimum);
        return frame::group_by(selected, "venue").agg("total"_n = operators::sum_(col("size")));
    };
    const auto maximum = std::numeric_limits<i64>::max();
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", maximum}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{1}}}},
        literal::SKIP,
        {{{"id", i64{2}}, {"size", i64{3}}}},
        literal::SKIP,
    });
    CHECK(run_graph(graph, updates, input<TS<i64>>({0, 2, literal::SKIP, maximum})).equals({
        {{{"venue", symbol{"X"}}, {"total", std::numeric_limits<i64>::min()}}},
        {{{"venue", symbol{"X"}}, {"total", maximum}}},
        {{{"venue", symbol{"X"}}, {"total", std::numeric_limits<i64>::min() + i64{2}}}},
        {literal::REMOVE({{"venue", symbol{"X"}}})},
    }));
}

struct RecyclingSource {
    In<TS<i64>> tick;
    State<row_id_t> row;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*tick) {
        case 0:
            *row = output.add(Row<Quotes>{.venue = symbol{"X"}, .size = 5});
            return;
        case 1:
            output.set(*row, RowPatch<Quotes>{.size = 10});
            return;
        case 2:
            output.erase(*row);
            *row = output.add(Row<Quotes>{.venue = symbol{"X"}, .size = 7});
            return;
        case 3:
            output.erase(*row);
            return;
        case 4:
            *row = output.add(Row<Quotes>{.venue = symbol{"X"}, .size = 9});
            return;
        default:
            return;
        }
    }
};

TEST_CASE("group rows keep their identity until the group is empty at invocation end",
          "[feature:operator][transition:update][transition:removal][feature:state]") {
    auto identities = std::vector<std::vector<row_id_t>>{};
    auto values = std::vector<std::vector<i64>>{};
    auto removals = std::vector<std::vector<row_id_t>>{};
    const auto source = node<RecyclingSource>();
    const auto observe = node([&](In<TSF<RESULT>> result, Out<TS<i64>> output) {
        auto rows = std::vector<row_id_t>{};
        auto totals = std::vector<i64>{};
        auto removed = std::vector<row_id_t>{};
        result.for_each_row([&](row_id_t id, std::size_t) {
            rows.push_back(id);
            totals.push_back(*result.column("total").input<i64>().cell(id));
        });
        result.for_each_removed([&](row_id_t id) { removed.push_back(id); });
        identities.push_back(std::move(rows));
        values.push_back(std::move(totals));
        removals.push_back(std::move(removed));
        output.set(static_cast<i64>(result.size()));
    });
    const auto graph = [&](wp<TS<i64>> tick) {
        return observe(frame::group_by(source(tick), "venue").agg("total"_n = operators::sum_(col("size"))));
    };
    CHECK(run_graph(graph, std::array<i64, 5>{0, 1, 2, 3, 4}).equals<i64>({1, 1, 1, 0, 1}));
    REQUIRE(identities.size() == 5);
    CHECK(identities[0] == identities[1]);
    CHECK(identities[0] == identities[2]);
    CHECK(removals[1].empty());
    CHECK(removals[2].empty());
    CHECK(removals[3] == identities[0]);
    CHECK(values == std::vector<std::vector<i64>>{{5}, {10}, {7}, {}, {9}});
}

TEST_CASE("grouped sums combine null grouping cells into one keyed row",
          "[feature:operator][transition:update][transition:removal]") {
    const auto graph = [](wp<TSF<Trades>> trades) {
        return frame::group_by(trades, "venue").agg("total"_n = operators::sum_(col("size")));
    };
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", NULL_VALUE}, {"size", i64{4}}},
         {{"id", i64{2}}, {"venue", NULL_VALUE}, {"size", i64{5}}}},
        {{{"id", i64{1}}, {"venue", symbol{"X"}}}},
        {literal::REMOVE({{"id", i64{2}}})},
    });
    CHECK(run_graph(graph, updates).equals({
        {{{"venue", NULL_VALUE}, {"total", i64{9}}}},
        {{{"venue", NULL_VALUE}, {"total", i64{5}}}, {{"venue", symbol{"X"}}, {"total", i64{4}}}},
        {literal::REMOVE({{"venue", NULL_VALUE}})},
    }));
}

TEST_CASE("grouped sums preserve group rows when columns rebind within the source row domain",
          "[feature:operator][feature:time-series-view][transition:update]") {
    auto identities = std::vector<row_id_t>{};
    auto removals = std::vector<row_id_t>{};
    const auto select = node([](In<TSF<Trades>> input, In<TS<bool>> use_id,
                                Out<TSF<Quotes>, view> output) {
        const auto indices = std::array<std::size_t, 2>{1, *use_id ? 0U : 2U};
        output.bind_projection(input, indices);
    });
    const auto observe = node([&](In<TSF<RESULT>> input, Out<TS<i64>> output) {
        REQUIRE(input.size() == 1);
        input.for_each_row([&](row_id_t id, std::size_t) {
            identities.push_back(id);
            output.set(*input.column("total").input<i64>().cell(id));
        });
        input.for_each_removed([&](row_id_t id) { removals.push_back(id); });
    });
    const auto graph = [&](wp<TSF<Trades>> input, wp<TS<bool>> use_id) {
        return observe(frame::group_by(select(input, use_id), "venue")
                           .agg("total"_n = operators::sum_(col("size"))));
    };
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{4}}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{5}}}},
        literal::SKIP,
        literal::SKIP,
    });
    CHECK(run_graph(graph, updates, std::array{false, true, false}).equals<i64>({9, 3, 9}));
    REQUIRE(identities.size() == 3);
    CHECK(identities[0] == identities[1]);
    CHECK(identities[0] == identities[2]);
    CHECK(removals.empty());
}

TEST_CASE("grouped sums recover after floating-point non-finite contributions and overflow",
          "[feature:operator][scalar:floating-point][transition:update][transition:removal]") {
    const auto maximum = std::numeric_limits<double>::max();
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto graph = [](wp<TSF<Trades>> trades) {
        const auto result = frame::group_by(trades, "venue").agg("total"_n = operators::sum_(col("price")));
        return operators::sum_(result["total"]).as<TS<double>>();
    };
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"price", maximum}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"price", maximum}},
         {{"id", i64{3}}, {"venue", symbol{"X"}}, {"price", -maximum}}},
        {literal::REMOVE({{"id", i64{2}}})},
        {literal::REMOVE({{"id", i64{3}}})},
        {{{"id", i64{1}}, {"price", infinity}}},
        {{{"id", i64{2}}, {"venue", symbol{"X"}}, {"price", -infinity}}},
        {literal::REMOVE({{"id", i64{2}}})},
        {{{"id", i64{1}}, {"price", nan}}},
        {{{"id", i64{1}}, {"price", 2.0}}},
    });
    const auto result = run_graph(graph, updates);
    REQUIRE(result.size() == 8);
    REQUIRE(result[0]);
    CHECK(result[0]->view().value() == infinity);
    REQUIRE(result[1]);
    CHECK(result[1]->view().value() == 0.0);
    REQUIRE(result[2]);
    CHECK(result[2]->view().value() == maximum);
    REQUIRE(result[3]);
    CHECK(result[3]->view().value() == infinity);
    REQUIRE(result[4]);
    CHECK(std::isnan(result[4]->view().value()));
    REQUIRE(result[5]);
    CHECK(result[5]->view().value() == infinity);
    REQUIRE(result[6]);
    CHECK(std::isnan(result[6]->view().value()));
    REQUIRE(result[7]);
    CHECK(result[7]->view().value() == 2.0);
}

TEST_CASE("grouped double sums compensate, recover, move, and reset finite contributions",
          "[feature:operator][scalar:floating-point][transition:update][transition:removal][feature:state]") {
    struct GroupValue {
        symbol venue;
        double total;
    };

    auto observed = std::vector<std::vector<GroupValue>>{};
    const auto observe = node([&](In<TSF<RESULT>> result, Out<TS<i64>> output) {
        auto groups = std::vector<GroupValue>{};
        const auto venues = result.column("venue").input<symbol>();
        const auto totals = result.column("total").input<double>();
        result.for_each_row([&](row_id_t id, std::size_t) {
            groups.push_back({*venues.cell(id), *totals.cell(id)});
        });
        observed.push_back(std::move(groups));
        output.set(static_cast<i64>(result.size()));
    });
    const auto graph = [&](wp<TSF<Trades>> input) {
        const auto grouped = frame::group_by(input, "venue").agg("total"_n = operators::sum_(col("price")));
        return observe(grouped);
    };
    const auto maximum = std::numeric_limits<double>::max();
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"price", maximum}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"price", maximum}},
         {{"id", i64{3}}, {"venue", symbol{"X"}}, {"price", -maximum}},
         {{"id", i64{4}}, {"venue", symbol{"Y"}}, {"price", maximum}},
         {{"id", i64{5}}, {"venue", symbol{"Y"}}, {"price", maximum}},
         {{"id", i64{6}}, {"venue", symbol{"Y"}}, {"price", -maximum}}},
        {literal::REMOVE({{"id", i64{2}}}), literal::REMOVE({{"id", i64{5}}})},
        {{{"id", i64{3}}, {"price", NULL_VALUE}},
         {{"id", i64{6}}, {"venue", symbol{"X"}}, {"price", -maximum / 2}}},
        {literal::REMOVE({{"id", i64{1}}}), literal::REMOVE({{"id", i64{3}}}),
         literal::REMOVE({{"id", i64{6}}})},
        {{{"id", i64{7}}, {"venue", symbol{"X"}}, {"price", 1e16}},
         {{"id", i64{8}}, {"venue", symbol{"X"}}, {"price", 1.0}},
         {{"id", i64{9}}, {"venue", symbol{"X"}}, {"price", -1e16}}},
    });

    CHECK(run_graph(graph, updates).equals<i64>({2, 2, 2, 1, 2}));
    REQUIRE(observed.size() == 5);
    const auto find_group = [](const auto &groups, symbol venue) {
        return std::ranges::find(groups, venue, &GroupValue::venue);
    };
    const auto x_initial = find_group(observed[0], symbol{"X"});
    const auto y_initial = find_group(observed[0], symbol{"Y"});
    REQUIRE(x_initial != observed[0].end());
    REQUIRE(y_initial != observed[0].end());
    CHECK(std::isinf(x_initial->total));
    CHECK(std::isinf(y_initial->total));

    const auto x_recovered = find_group(observed[1], symbol{"X"});
    const auto y_recovered = find_group(observed[1], symbol{"Y"});
    REQUIRE(x_recovered != observed[1].end());
    REQUIRE(y_recovered != observed[1].end());
    CHECK(x_recovered->total == 0.0);
    CHECK(y_recovered->total == 0.0);

    const auto x_moved = find_group(observed[2], symbol{"X"});
    const auto y_moved = find_group(observed[2], symbol{"Y"});
    REQUIRE(x_moved != observed[2].end());
    REQUIRE(y_moved != observed[2].end());
    CHECK(x_moved->total == maximum / 2);
    CHECK(y_moved->total == maximum);
    CHECK(find_group(observed[3], symbol{"X"}) == observed[3].end());
    const auto y_after_removal = find_group(observed[3], symbol{"Y"});
    REQUIRE(y_after_removal != observed[3].end());
    CHECK(y_after_removal->total == maximum);

    const auto x_recreated = find_group(observed[4], symbol{"X"});
    const auto y_retained = find_group(observed[4], symbol{"Y"});
    REQUIRE(x_recreated != observed[4].end());
    REQUIRE(y_retained != observed[4].end());
    CHECK(x_recreated->total == 1.0);
    CHECK(y_retained->total == maximum);
}

TEST_CASE("grouped double sums retain small contributions after removals and null updates",
          "[feature:operator][scalar:floating-point][transition:update][transition:removal]") {
    const auto observe = node([](In<TSF<RESULT>> grouped, Out<TS<double>> output) {
        REQUIRE(grouped.size() == 1);
        grouped.for_each_row([&](row_id_t id, std::size_t) {
            output.set(*grouped.column("total").input<double>().cell(id));
        });
    });
    const auto graph = [&](wp<TSF<Trades>> input) {
        return observe(frame::group_by(input, "venue").agg("total"_n = operators::sum_(col("price"))));
    };
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"price", 1e16}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"price", 1.0}}},
        {literal::REMOVE({{"id", i64{1}}})},
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"price", 1e16}}},
        {{{"id", i64{1}}, {"price", NULL_VALUE}}},
    });

    CHECK(run_graph(graph, updates).equals<double>({1e16, 1.0, 1e16, 1.0}));
}

TEST_CASE("grouped double sum retains compensation across a transient combined overflow",
          "[feature:operator][scalar:floating-point][transition:addition]") {
    const auto graph = [](wp<TSF<Trades>> trades) {
        const auto grouped = frame::group_by(trades, "venue").agg("total"_n = operators::sum_(col("price")));
        return operators::sum_(grouped["total"]).as<TS<double>>();
    };
    const auto maximum = std::numeric_limits<double>::max();
    const auto quarter_ulp = std::ldexp(1.0, 969);
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"price", maximum}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"price", quarter_ulp}},
         {{"id", i64{3}}, {"venue", symbol{"X"}}, {"price", quarter_ulp}},
         {{"id", i64{4}}, {"venue", symbol{"X"}}, {"price", -quarter_ulp}}},
    });

    CHECK(run_graph(graph, updates).equals<double>({maximum}));
}

TEST_CASE("grouped sums accept wiring-time aggregation names and independent graph instances",
          "[feature:operator][schema:unnamed][declaration:runtime][feature:state]") {
    const auto graph = [](wp<TSF<Trades>> trades) {
        const auto name = symbol{"quantity"};
        const auto sum = operators::sum_(col("size"));
        const auto assignments = std::array{WiringArgument{name, sum}};
        return frame::group_by(trades, "venue").agg(std::span<const WiringArgument>{assignments});
    };
    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{4}}}},
        {{{"id", i64{1}}, {"size", i64{8}}}},
    });
    CHECK(run_graph(graph, updates).equals({
        {{{"venue", symbol{"X"}}, {"quantity", i64{4}}}},
        {{{"venue", symbol{"X"}}, {"quantity", i64{8}}}},
    }));
    CHECK(run_graph(graph, updates).equals({
        {{{"venue", symbol{"X"}}, {"quantity", i64{4}}}},
        {{{"venue", symbol{"X"}}, {"quantity", i64{8}}}},
    }));
}
// A separately supplied aggregation exercises the extension contract with a nonnumeric operand.
class NonNullAggregation {
  public:
    explicit NonNullAggregation(allocator) {}

    static inline std::size_t updated_rows{};
    static inline std::size_t moved_rows{};

    void add_group(std::uint32_t group) { counts_.emplace(group, 0); }
    void erase_group(std::uint32_t group) { counts_.erase(group); }
    void remove(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups) {
        for (auto position = std::size_t{}; position != rows.size(); ++position)
            if (valid_.erase(rows[position]))
                --counts_.at(groups[position]);
    }
    void move(std::span<const row_id_t> rows, std::span<const std::uint32_t> previous,
              std::span<const std::uint32_t> groups) {
        moved_rows += rows.size();
        for (auto position = std::size_t{}; position != rows.size(); ++position) {
            if (!valid_.contains(rows[position]))
                continue;
            --counts_.at(previous[position]);
            ++counts_.at(groups[position]);
        }
    }
    void update(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups,
                const ColumnBatch<string> &values) {
        updated_rows += rows.size();
        remove(rows, groups);
        for (auto position = std::size_t{}; position != rows.size(); ++position) {
            if (!values.is_valid(position))
                continue;
            valid_.insert(rows[position]);
            ++counts_.at(groups[position]);
        }
    }
    i64 result(std::uint32_t group, const AggregationRows &) {
        return counts_.at(group);
    }

  private:
    std::unordered_set<row_id_t> valid_;
    std::unordered_map<std::uint32_t, i64> counts_;
};

const ScalarDispatch<AggregationFunctionAdapter> &nonnull_kernels() {
    static auto registry = ScalarDispatch<AggregationFunctionAdapter>{};
    static const auto registered = [&] {
        static_cast<void>(register_aggregation_function<i64, string, NonNullAggregation>(registry));
        return true;
    }();
    static_cast<void>(registered);
    return registry;
}

TEST_CASE("aggregation kernels update only changed operands and move cached contributions on key changes",
          "[feature:operator][transition:update][transition:removal][scalar:string][feature:state]") {
    struct GroupValues {
        symbol venue;
        i64 total;
        i64 notes;
    };

    NonNullAggregation::updated_rows = 0;
    NonNullAggregation::moved_rows = 0;
    auto processed = std::vector<std::size_t>{};
    auto moved = std::vector<std::size_t>{};
    auto changed_columns = std::vector<std::array<bool, 3>>{};
    auto observed = std::vector<std::vector<GroupValues>>{};
    const auto observe = node([&](In<TSF<RESULT>> input, Out<TS<i64>> output) {
        processed.push_back(NonNullAggregation::updated_rows);
        moved.push_back(NonNullAggregation::moved_rows);
        changed_columns.push_back({input.column("venue").modified(), input.column("total").modified(),
                                   input.column("notes").modified()});
        auto rows = std::vector<GroupValues>{};
        const auto venues = input.column("venue").input<symbol>();
        const auto totals = input.column("total").input<i64>();
        const auto notes = input.column("notes").input<i64>();
        input.for_each_row([&](row_id_t id, std::size_t) {
            rows.push_back({*venues.cell(id), *totals.cell(id), *notes.cell(id)});
        });
        observed.push_back(std::move(rows));
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TSF<Trades>> input) {
        const auto count = bundle<AggregationExpression>(
            "operation"_n = ScalarValue<AggregationOperation>{&nonnull_kernels()}, "operand"_n = col("note"));
        const auto result = frame::group_by(input, "venue").agg(
            "total"_n = operators::sum_(col("size")), "notes"_n = count);
        return observe(result);
    };
    CHECK(run_graph(graph, input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}, {"note", "one"}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{7}}, {"note", NULL_VALUE}},
         {{"id", i64{3}}, {"venue", symbol{"Y"}}, {"size", i64{9}}, {"note", "three"}}},
        {{{"id", i64{1}}, {"size", i64{8}}}},
        {{{"id", i64{2}}, {"note", "two"}}},
        {{{"id", i64{1}}, {"venue", symbol{"Y"}}}},
        {literal::REMOVE({{"id", i64{1}}})},
    })).equals<i64>({2, 2, 2, 2, 2}));
    CHECK(processed == std::vector<std::size_t>{3, 3, 4, 4, 4});
    CHECK(moved == std::vector<std::size_t>{0, 0, 0, 1, 1});
    REQUIRE(changed_columns.size() == 5);
    CHECK(changed_columns[1] == std::array{false, true, false});
    CHECK(changed_columns[2] == std::array{false, false, true});
    REQUIRE(observed.size() == 5);
    const auto check_group = [](const auto &groups, symbol venue, i64 total, i64 notes) {
        const auto found = std::ranges::find(groups, venue, &GroupValues::venue);
        REQUIRE(found != groups.end());
        CHECK(found->total == total);
        CHECK(found->notes == notes);
    };
    check_group(observed[0], symbol{"X"}, 12, 1);
    check_group(observed[0], symbol{"Y"}, 9, 1);
    check_group(observed[1], symbol{"X"}, 15, 1);
    check_group(observed[1], symbol{"Y"}, 9, 1);
    check_group(observed[2], symbol{"X"}, 15, 2);
    check_group(observed[2], symbol{"Y"}, 9, 1);
    check_group(observed[3], symbol{"X"}, 7, 1);
    check_group(observed[3], symbol{"Y"}, 17, 2);
    check_group(observed[4], symbol{"X"}, 7, 1);
    check_group(observed[4], symbol{"Y"}, 9, 1);
}

class CountLabelAggregation : public NonNullAggregation {
  public:
    using NonNullAggregation::NonNullAggregation;

    std::optional<std::string_view> result(std::uint32_t group, const AggregationRows &membership) {
        const auto count = NonNullAggregation::result(group, membership);
        if (count == 0)
            return std::nullopt;
        label_ = std::to_string(count) + " non-null values in this group";
        return label_;
    }

  private:
    std::string label_;
};

TEST_CASE("aggregation registration reports duplicates and retains the first handler", "[feature:dispatch]") {
    auto functions = ScalarDispatch<AggregationFunctionAdapter>{};
    CHECK(register_aggregation_function<i64, string, NonNullAggregation>(functions));
    const auto *first = functions.find(cpp_scalar_type<string>());
    REQUIRE(first != nullptr);
    const auto original = *first;

    CHECK_FALSE(register_aggregation_function<string, string, CountLabelAggregation>(functions));
    const auto *registered = functions.find(cpp_scalar_type<string>());
    REQUIRE(registered != nullptr);
    CHECK(registered->result_type == original.result_type);
    CHECK(registered->create == original.create);
    CHECK(registered->result_type == &cpp_scalar_type<i64>());
}

TEST_CASE("aggregation adapters own nullable string results across groups and ticks",
          "[feature:dispatch][feature:operator][scalar:string][transition:update][transition:removal]") {
    auto functions = ScalarDispatch<AggregationFunctionAdapter>{};
    static_cast<void>(register_aggregation_function<string, string, CountLabelAggregation>(functions));
    const auto graph = [&](wp<TSF<Trades>> input) {
        const auto label = bundle<AggregationExpression>(
            "operation"_n = ScalarValue<AggregationOperation>{&functions}, "operand"_n = col("note"));
        return frame::group_by(input, "venue").agg("label"_n = label);
    };
    CHECK(run_graph(graph, input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"note", "one"}},
         {{"id", i64{2}}, {"venue", symbol{"Y"}}, {"note", "two"}},
         {{"id", i64{3}}, {"venue", symbol{"Y"}}, {"note", "three"}},
         {{"id", i64{4}}, {"venue", symbol{"Z"}}, {"note", NULL_VALUE}}},
        {{{"id", i64{1}}, {"note", NULL_VALUE}}, {{"id", i64{4}}, {"note", "four"}}},
        {literal::REMOVE({{"id", i64{3}}})},
    })).equals({
        {{{"venue", symbol{"X"}}, {"label", "1 non-null values in this group"}},
         {{"venue", symbol{"Y"}}, {"label", "2 non-null values in this group"}},
         {{"venue", symbol{"Z"}}, {"label", NULL_VALUE}}},
        {{{"venue", symbol{"X"}}, {"label", NULL_VALUE}},
         {{"venue", symbol{"Z"}}, {"label", "1 non-null values in this group"}}},
        {{{"venue", symbol{"Y"}}, {"label", "1 non-null values in this group"}}},
    }));
}

TEST_CASE("aggregation expressions execute inside the aggregation node",
          "[feature:operator][feature:wiring][schema:unnamed][declaration:runtime]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source = node([](Out<TSF<Trades>>) {});
    const auto grouped = frame::group_by(source(), "venue").agg(
        "notional"_n = operators::sum_(col("price") * frame::convert<double>(col("size"))));
    wiring.require_materialisation(grouped, grouped.type());
    wiring.finalize();
    for (const auto &node : wiring.nodes())
        CHECK(dynamic_cast<const ColumnSeriesType *>(node.signature().output) == nullptr);
}


TEST_CASE("grouped and column sums handle swapped source row IDs without replacing groups",
          "[feature:operator][series:reference][feature:sampling][transition:update]") {
    const auto choose = node([](In<TS<bool>> select, In<REF<TSF<SCHEMA>>> first,
                                In<REF<TSF<SCHEMA>>> second, Out<REF<TSF<SCHEMA>>> output) {
        output.copy(*select ? second : first);
    });
    auto row_ids = std::vector<row_id_t>{};
    auto removals = std::vector<row_id_t>{};
    const auto observe = node([&](In<TSF<RESULT>> grouped, In<TS<i64>> total, Out<TS<i64>> output) {
        REQUIRE(grouped.size() == 1);
        grouped.for_each_row([&](row_id_t id, std::size_t) {
            row_ids.push_back(id);
            CHECK(grouped.column("total").input<i64>().cell(id) == *total);
        });
        grouped.for_each_removed([&](row_id_t id) { removals.push_back(id); });
        output.set(*total);
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TSF<SCHEMA>> first, wp<TSF<SCHEMA>> second) {
        const auto selected = choose(select, first, second);
        const auto grouped = frame::group_by(selected, "venue").agg("total"_n = operators::sum_(col("size")));
        const auto total = operators::sum_(select_column<i64>(selected, symbol{"size"}));
        return observe(grouped, total);
    };
    const auto first = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{7}}}},
        literal::SKIP, literal::SKIP, {{{"id", i64{1}}, {"size", i64{6}}}}});
    const auto second = input<TSF<Trades>>({
        {{{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{11}}},
         {{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{13}}}},
        literal::SKIP, literal::SKIP, literal::SKIP});
    CHECK(run_graph(graph, std::array{false, true, false, false}, first, second).equals<i64>({12, 24, 12, 13}));
    REQUIRE(row_ids.size() == 4);
    CHECK(row_ids[0] == row_ids[1]);
    CHECK(row_ids[0] == row_ids[2]);
    CHECK(row_ids[0] == row_ids[3]);
    CHECK(removals.empty());
}

TEST_CASE("group-by remapping keeps common groups and updates changed group membership",
          "[feature:operator][series:reference][feature:sampling][transition:update][transition:removal]") {
    struct GroupRow {
        symbol venue;
        i64 total;
        row_id_t row;
    };

    auto observed = std::vector<std::vector<GroupRow>>{};
    auto removed = std::vector<std::vector<row_id_t>>{};
    auto source_row_ids = std::vector<std::array<row_id_t, 2>>{};
    const auto choose = node([](In<TS<bool>> select, In<REF<TSF<SCHEMA>>> first,
                                In<REF<TSF<SCHEMA>>> second, Out<REF<TSF<SCHEMA>>> output) {
        output.copy(*select ? second : first);
    });
    const auto observe = node([&](In<TSF<Trades>> source, In<TSF<RESULT>> grouped, Out<TS<i64>> output) {
        auto ids = std::array<row_id_t, 2>{};
        const auto source_keys = source.column("id").input<i64>();
        source.for_each_row([&](row_id_t id, std::size_t) {
            const auto key = *source_keys.cell(id);
            ids[static_cast<std::size_t>(key - 1)] = id;
        });
        source_row_ids.push_back(ids);

        auto rows = std::vector<GroupRow>{};
        const auto venues = grouped.column("venue").input<symbol>();
        const auto totals = grouped.column("total").input<i64>();
        grouped.for_each_row([&](row_id_t id, std::size_t) {
            rows.push_back({*venues.cell(id), *totals.cell(id), id});
        });
        observed.push_back(std::move(rows));

        auto removed_rows = std::vector<row_id_t>{};
        grouped.for_each_removed([&](row_id_t id) { removed_rows.push_back(id); });
        removed.push_back(std::move(removed_rows));
        output.set(static_cast<i64>(grouped.size()));
    });
    const auto graph = [&](wp<TS<bool>> select, wp<TSF<SCHEMA>> first, wp<TSF<SCHEMA>> second) {
        const auto source = choose(select, first, second);
        const auto grouped = frame::group_by(source, "venue").agg("total"_n = operators::sum_(col("size")));
        return observe(source, grouped);
    };
    const auto first = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{5}}},
         {{"id", i64{2}}, {"venue", symbol{"Y"}}, {"size", i64{7}}}},
        literal::SKIP,
        literal::SKIP,
    });
    const auto second = input<TSF<Trades>>({
        {{{"id", i64{2}}, {"venue", symbol{"Z"}}, {"size", i64{13}}},
         {{"id", i64{1}}, {"venue", symbol{"Y"}}, {"size", i64{11}}}},
        literal::SKIP,
        literal::SKIP,
    });

    CHECK(run_graph(graph, std::array{false, true, false}, first, second).equals<i64>({2, 2, 2}));
    REQUIRE(observed.size() == 3);
    REQUIRE(source_row_ids.size() == 3);
    CHECK(source_row_ids[0][0] == source_row_ids[1][1]);
    CHECK(source_row_ids[0][1] == source_row_ids[1][0]);
    CHECK(source_row_ids[0][0] == source_row_ids[2][0]);
    CHECK(source_row_ids[0][1] == source_row_ids[2][1]);

    const auto find_group = [](const auto &groups, symbol venue) {
        return std::ranges::find(groups, venue, &GroupRow::venue);
    };
    const auto common_first = find_group(observed[0], symbol{"Y"});
    const auto common_second = find_group(observed[1], symbol{"Y"});
    const auto common_third = find_group(observed[2], symbol{"Y"});
    REQUIRE(common_first != observed[0].end());
    REQUIRE(common_second != observed[1].end());
    REQUIRE(common_third != observed[2].end());
    CHECK(common_first->total == 7);
    CHECK(common_second->total == 11);
    CHECK(common_third->total == 7);
    CHECK(common_first->row == common_second->row);
    CHECK(common_first->row == common_third->row);

    const auto first_x = find_group(observed[0], symbol{"X"});
    REQUIRE(first_x != observed[0].end());
    CHECK(first_x->total == 5);
    CHECK(find_group(observed[1], symbol{"X"}) == observed[1].end());
    const auto second_z = find_group(observed[1], symbol{"Z"});
    REQUIRE(second_z != observed[1].end());
    CHECK(second_z->total == 13);
    const auto third_x = find_group(observed[2], symbol{"X"});
    REQUIRE(third_x != observed[2].end());
    CHECK(third_x->total == 5);
    CHECK(find_group(observed[2], symbol{"Z"}) == observed[2].end());
    REQUIRE(removed.size() == 3);
    REQUIRE(removed[1].size() == 1);
    CHECK(removed[1][0] == first_x->row);
    REQUIRE(removed[2].size() == 1);
    CHECK(removed[2][0] == second_z->row);
}

TEST_CASE("grouped aggregates match a full recomputation after mixed mutations",
          "[feature:operator][transition:addition][transition:update][transition:removal]"
          "[scalar:string][feature:state]") {
    struct ModelRow {
        i64 id;
        std::optional<symbol> venue;
        std::optional<i64> size;
        std::optional<std::string> note;
    };
    struct GroupValue {
        std::optional<symbol> venue;
        i64 total;
        i64 notes;
    };

    const auto full_recompute = [](const std::vector<ModelRow> &rows) {
        auto groups = std::vector<GroupValue>{};
        for (const auto &row : rows) {
            auto group = std::ranges::find(groups, row.venue, &GroupValue::venue);
            if (group == groups.end()) {
                groups.push_back({row.venue, 0, 0});
                group = std::prev(groups.end());
            }
            if (row.size)
                group->total += *row.size;
            if (row.note)
                ++group->notes;
        }
        return groups;
    };

    auto observed = std::vector<std::vector<GroupValue>>{};
    const auto graph = [](wp<TSF<Trades>> input) {
        const auto notes = bundle<AggregationExpression>(
            "operation"_n = ScalarValue<AggregationOperation>{&nonnull_kernels()}, "operand"_n = col("note"));
        return frame::group_by(input, "venue").agg("total"_n = operators::sum_(col("size")), "notes"_n = notes);
    };
    const auto observe = node([&](In<TSF<RESULT>> input, Out<TS<i64>> output) {
        auto groups = std::vector<GroupValue>{};
        const auto venues = input.column("venue").input<symbol>();
        const auto totals = input.column("total").input<i64>();
        const auto notes = input.column("notes").input<i64>();
        input.for_each_row([&](row_id_t id, std::size_t) {
            groups.push_back({venues.cell(id), *totals.cell(id), *notes.cell(id)});
        });
        observed.push_back(std::move(groups));
        output.set(static_cast<i64>(input.size()));
    });
    const auto observed_graph = [&](wp<TSF<Trades>> input) { return observe(graph(input)); };

    const auto updates = input<TSF<Trades>>({
        {{{"id", i64{1}}, {"venue", symbol{"X"}}, {"size", i64{2}}, {"note", "a"}},
         {{"id", i64{2}}, {"venue", NULL_VALUE}, {"size", NULL_VALUE}, {"note", NULL_VALUE}},
         {{"id", i64{3}}, {"venue", symbol{"Y"}}, {"size", i64{4}}, {"note", "b"}}},
        {{{"id", i64{1}}, {"venue", symbol{"Y"}}, {"size", i64{6}}},
         {{"id", i64{2}}, {"venue", symbol{"X"}}, {"size", i64{3}}, {"note", "c"}},
         {{"id", i64{4}}, {"venue", symbol{"X"}}, {"size", i64{-1}}, {"note", NULL_VALUE}}},
        {{{"id", i64{1}}, {"size", i64{7}}, {"note", NULL_VALUE}},
         literal::REMOVE({{"id", i64{3}}}),
         literal::REMOVE({{"id", i64{4}}})},
        {{{"id", i64{2}}, {"venue", NULL_VALUE}, {"size", NULL_VALUE}, {"note", NULL_VALUE}},
         {{"id", i64{3}}, {"venue", NULL_VALUE}, {"size", i64{5}}, {"note", "d"}}},
        {literal::REMOVE({{"id", i64{1}}}),
         literal::REMOVE({{"id", i64{3}}}),
         {{"id", i64{2}}, {"venue", symbol{"Y"}}, {"size", i64{8}}, {"note", "f"}},
         {{"id", i64{5}}, {"venue", symbol{"Y"}}, {"size", i64{-2}}, {"note", "e"}}},
    });
    const auto full_rows = std::array{
        std::vector<ModelRow>{{1, symbol{"X"}, 2, std::string{"a"}},
                              {2, std::nullopt, std::nullopt, std::nullopt},
                              {3, symbol{"Y"}, 4, std::string{"b"}}},
        std::vector<ModelRow>{{1, symbol{"Y"}, 6, std::string{"a"}},
                              {2, symbol{"X"}, 3, std::string{"c"}},
                              {3, symbol{"Y"}, 4, std::string{"b"}},
                              {4, symbol{"X"}, -1, std::nullopt}},
        std::vector<ModelRow>{{1, symbol{"Y"}, 7, std::nullopt}, {2, symbol{"X"}, 3, std::string{"c"}}},
        std::vector<ModelRow>{{1, symbol{"Y"}, 7, std::nullopt},
                              {2, std::nullopt, std::nullopt, std::nullopt},
                              {3, std::nullopt, 5, std::string{"d"}}},
        std::vector<ModelRow>{{2, symbol{"Y"}, 8, std::string{"f"}}, {5, symbol{"Y"}, -2, std::string{"e"}}},
    };

    CHECK(run_graph(observed_graph, updates).equals<i64>({3, 2, 2, 2, 1}));
    REQUIRE(observed.size() == full_rows.size());
    for (auto tick = std::size_t{}; tick != full_rows.size(); ++tick) {
        const auto expected = full_recompute(full_rows[tick]);
        REQUIRE(observed[tick].size() == expected.size());
        for (const auto &group : expected) {
            const auto actual = std::ranges::find(observed[tick], group.venue, &GroupValue::venue);
            REQUIRE(actual != observed[tick].end());
            CHECK(actual->total == group.total);
            CHECK(actual->notes == group.notes);
        }
    }
}

} // namespace
