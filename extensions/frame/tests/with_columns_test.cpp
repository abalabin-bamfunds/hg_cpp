#include <catch2/catch_test_macros.hpp>

#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/graph.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/list/scalar.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<i64> id;
    Field<double> price;
};

template <template <typename, auto...> typename Field> struct KeyedQuotes {
    Field<string> id;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field> struct EmptySchema {};

template <template <typename, auto...> typename Field>
using SourceRuntimeSchema = SchemaVariableDeclaration<"WITH_COLUMNS_TEST_SOURCE_SCHEMA">;
template <template <typename, auto...> typename Field>
using ResultRuntimeSchema = SchemaVariableDeclaration<"WITH_COLUMNS_TEST_RESULT_SCHEMA">;
template <template <typename, auto...> typename Field>
using UnchangedRuntimeSchema = SchemaVariableDeclaration<"WITH_COLUMNS_TEST_UNCHANGED_SCHEMA">;

struct SourceState {
    row_id_t row{no_row};
};

struct QuoteSource {
    In<TS<i64>> command;
    State<SourceState> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        if (*command == 1)
            state->row = output.add(Row<Quotes>{.id = 7, .price = 8.0});
        if (*command == 2)
            output.set(state->row, RowPatch<Quotes>{.price = 12.0});
        if (*command == 3)
            output.set(state->row, RowPatch<Quotes>{.price = 8.0});
    }
};

struct BroadcastValueSource {
    In<TS<i64>> command;
    Out<TS<i64>> output;

    void evaluate() const {
        switch (*command) {
        case 1:
            output.set(5);
            return;
        case 2:
            output.invalidate();
            return;
        case 3:
            output.set(9);
            return;
        default:
            return;
        }
    }
};

struct Observation {
    bool valid{};
    bool shares_row_domain{};
    std::vector<std::string> names;
    std::vector<std::optional<i64>> ids;
    std::vector<std::optional<bool>> prices;
    std::vector<std::optional<double>> doubled_prices;
};

struct RuntimeObservation {
    bool valid{};
    bool shares_row_domain{};
    bool unchanged_valid{};
    bool unchanged_shares_row_domain{};
    std::vector<std::string> names;
    std::vector<std::optional<double>> prices;
    std::vector<std::optional<double>> copies;
    std::vector<std::optional<double>> unchanged_prices;
    std::vector<std::optional<i64>> runtime_bases;
    std::vector<std::optional<i64>> runtime_constants;
};

struct EmptySourceState {
    row_id_t first{no_row};
    row_id_t second{no_row};
};

struct EmptySource {
    In<TS<i64>> command;
    State<EmptySourceState> state;
    Out<TSF<EmptySchema>> output;

    void evaluate() const {
        switch (*command) {
        case 1:
            state->first = output.add(EmptySchema<Cell>{});
            return;
        case 2:
            state->second = output.add(EmptySchema<Cell>{});
            return;
        case 3:
            CHECK(output.erase(state->first));
            return;
        case 4: {
            const auto reused = output.add(EmptySchema<Cell>{});
            CHECK(reused == state->first);
            return;
        }
        case 5:
            output.invalidate();
            return;
        case 6:
            state->first = output.add(EmptySchema<Cell>{});
            return;
        default:
            return;
        }
    }
};

struct ManyRowsSource {
    In<TS<i64>> command;
    Out<TSF<EmptySchema>> output;

    void evaluate() const {
        if (*command == 1)
            for (auto index = std::size_t{}; index != 129; ++index)
                static_cast<void>(output.add(EmptySchema<Cell>{}));
    }
};

struct BroadcastRow {
    row_id_t id{};
    std::optional<bool> bool_series;
    std::optional<bool> bool_constant;
    std::optional<i64> i64_series;
    std::optional<i64> i64_constant;
    std::optional<double> double_series;
    std::optional<double> double_constant;
    std::optional<std::string> string_series;
    std::optional<std::string> string_constant;
    std::optional<symbol> symbol_series;
    std::optional<symbol> symbol_constant;
};

struct BroadcastObservation {
    bool valid{};
    std::vector<std::string> names;
    std::vector<BroadcastRow> rows;
};

struct BorrowedBroadcastObservation {
    bool valid{};
    bool frame_modified{};
    bool pass_through_is_borrowed{};
    bool broadcast_modified{};
    std::vector<row_id_t> broadcast_changed;
    std::vector<std::optional<i64>> source_values;
    std::vector<std::optional<i64>> broadcast_values;
};

TEST_CASE("with_columns infers its result schema in an enclosing graph template",
          "[feature:frame][feature:with_columns]") {
    auto observations = std::vector<Observation>{};
    const auto source_node = node<QuoteSource>();
    const auto transform = graph([](wp<TSF<SCHEMA>> source) {
        return frame::with_columns(source, "doubled"_n = col("price") * 2.0,
                                   "price"_n = col("price") > 10.0);
    });
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<SCHEMA>> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = Observation{.valid = result.valid()};
        if (result.valid()) {
            observation.shares_row_domain = source.row_domain() == result.row_domain();
            for (const auto &field : result.schema().fields())
                observation.names.emplace_back(field.name.text());
            result.for_each_row([&](row_id_t id, std::size_t) {
                observation.ids.push_back(result.column("id").input<i64>().cell(id));
                observation.prices.push_back(result.column("price").input<bool>().cell(id));
                observation.doubled_prices.push_back(result.column("doubled").input<double>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto run = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        return observe(source, transform(source), tick);
    };

    const auto result = run_graph(run, input<TS<i64>>({1, 2}), input<TS<i64>>({0, 1}));

    REQUIRE(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].shares_row_domain);
    CHECK(observations[0].names == std::vector<std::string>{"id", "price", "doubled"});
    CHECK(observations[0].ids == std::vector<std::optional<i64>>{7});
    CHECK(observations[0].prices == std::vector<std::optional<bool>>{false});
    CHECK(observations[0].doubled_prices == std::vector<std::optional<double>>{16.0});
    CHECK(observations[1].prices == std::vector<std::optional<bool>>{true});
    CHECK(observations[1].doubled_prices == std::vector<std::optional<double>>{24.0});
}

TEST_CASE("with_columns rejects duplicate assignment names and keyed field replacement",
          "[feature:frame][error:wiring]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<KeyedQuotes>>) {});
    const auto source = source_node();
    const auto capture_wiring_error = [](auto &&operation) {
        auto error = std::optional<WiringError>{};
        try {
            operation();
        } catch (const WiringError &caught) {
            error = caught;
        }
        return error;
    };

    const auto duplicate_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns(
            source, "price"_n = select_column<double>(source, util::symbol{"price"}),
            "price"_n = select_column<double>(source, util::symbol{"price"})));
    });
    REQUIRE(duplicate_error);
    CHECK(std::string_view{duplicate_error->what()} == "a schema has duplicate field names");

    const auto key_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns(source, "id"_n = select_column<string>(source, util::symbol{"id"})));
    });
    REQUIRE(key_error);
    CHECK(std::string_view{key_error->what()}.contains("frame::with_columns cannot assign a key column"));
}

TEST_CASE("with_columns accepts span assignments with an anonymous source schema",
          "[feature:frame][feature:with_columns]") {
    const auto fields = std::array{SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &schema = anonymous_schema(fields);
    const auto &runtime = frame_time_series_runtime_type<SCHEMA>(
        frame_series_type(schema, TimeSeriesGuarantee::ordinary));
    auto observations = std::vector<RuntimeObservation>{};
    const auto observe = node([&](In<TSF<SourceRuntimeSchema>, optional> source,
                                  In<TSF<ResultRuntimeSchema>, optional> result,
                                  In<TSF<UnchangedRuntimeSchema>, optional> unchanged, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = RuntimeObservation{.valid = result.valid(), .unchanged_valid = unchanged.valid()};
        if (result.valid()) {
            observation.shares_row_domain = source.row_domain() == result.row_domain();
            for (const auto &field : result.schema().fields())
                observation.names.emplace_back(field.name.text());
            result.for_each_row([&](row_id_t id, std::size_t) {
                observation.prices.push_back(result.column("price").input<double>().cell(id));
                observation.copies.push_back(result.column("copy").input<double>().cell(id));
            });
        }
        if (unchanged.valid()) {
            observation.unchanged_shares_row_domain = source.row_domain() == unchanged.row_domain();
            for (const auto &field : unchanged.schema().fields())
                observation.names.emplace_back("unchanged:" + std::string{field.name.text()});
            unchanged.for_each_row([&](row_id_t id, std::size_t) {
                observation.unchanged_prices.push_back(unchanged.column("price").input<double>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<SCHEMA>> source, wp<TS<i64>> tick) {
        const auto selected = select_column<double>(source, util::symbol{"price"});
        const auto assignments = std::array{WiringArgument{util::symbol{"copy"}, selected}};
        const auto result = frame::with_columns(source, std::span<const WiringArgument>{assignments});
        const auto unchanged = frame::with_columns(source, std::span<const WiringArgument>{});
        return observe(source, result, unchanged, tick);
    };

    const auto result = run_graph(
        graph,
        input(runtime, {literal::SKIP, {{{"price", 10.0}}}, {{{"price", 20.0}}}}),
        input<TS<i64>>({0, 1, 2}));

    REQUIRE(result.equals<i64>({0, 1, 2}));
    REQUIRE(observations.size() == 3);
    CHECK_FALSE(observations[0].valid);
    CHECK_FALSE(observations[0].unchanged_valid);
    CHECK(observations[1].valid);
    CHECK(observations[1].shares_row_domain);
    CHECK(observations[1].unchanged_valid);
    CHECK(observations[1].unchanged_shares_row_domain);
    CHECK(observations[1].names == std::vector<std::string>{"price", "copy", "unchanged:price"});
    CHECK(observations[1].prices == std::vector<std::optional<double>>{10.0});
    CHECK(observations[1].copies == std::vector<std::optional<double>>{10.0});
    CHECK(observations[1].unchanged_prices == std::vector<std::optional<double>>{10.0});
    CHECK(observations[2].unchanged_valid);
    CHECK(observations[2].unchanged_shares_row_domain);
    CHECK(observations[2].prices == (std::vector<std::optional<double>>{10.0, 20.0}));
    CHECK(observations[2].copies == (std::vector<std::optional<double>>{10.0, 20.0}));
    CHECK(observations[2].unchanged_prices == (std::vector<std::optional<double>>{10.0, 20.0}));
}

TEST_CASE("with_columns broadcasts scalar values and TS values over zero-field frame membership",
          "[feature:frame][feature:with_columns][declaration:generic]") {
    auto observations = std::vector<BroadcastObservation>{};
    const auto source_node = node<EmptySource>();
    const auto observe = node([&](In<TSF<EmptySchema>, optional> source,
                                  In<TSF<ResultRuntimeSchema>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = BroadcastObservation{.valid = result.valid()};
        if (result.valid()) {
            for (const auto &field : result.schema().fields())
                observation.names.emplace_back(field.name.text());

            const auto bool_series = result.column("bool_series").input<bool>();
            const auto bool_constant = result.column("bool_constant").input<bool>();
            const auto i64_series = result.column("i64_series").input<i64>();
            const auto i64_constant = result.column("i64_constant").input<i64>();
            const auto double_series = result.column("double_series").input<double>();
            const auto double_constant = result.column("double_constant").input<double>();
            const auto string_series = result.column("string_series").input<string>();
            const auto string_constant = result.column("string_constant").input<string>();
            const auto symbol_series = result.column("symbol_series").input<symbol>();
            const auto symbol_constant = result.column("symbol_constant").input<symbol>();
            result.for_each_row([&](row_id_t id, std::size_t) {
                const auto series_text = string_series.cell(id);
                const auto constant_text = string_constant.cell(id);
                observation.rows.push_back(BroadcastRow{
                    .id = id,
                    .bool_series = bool_series.cell(id),
                    .bool_constant = bool_constant.cell(id),
                    .i64_series = i64_series.cell(id),
                    .i64_constant = i64_constant.cell(id),
                    .double_series = double_series.cell(id),
                    .double_constant = double_constant.cell(id),
                    .string_series = series_text ? std::optional{std::string{*series_text}} : std::nullopt,
                    .string_constant = constant_text ? std::optional{std::string{*constant_text}} : std::nullopt,
                    .symbol_series = symbol_series.cell(id),
                    .symbol_constant = symbol_constant.cell(id),
                });
            });
            std::ranges::sort(observation.rows, {}, &BroadcastRow::id);
        }
        static_cast<void>(source);
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<bool>> bool_series, wp<TS<i64>> i64_series,
                           wp<TS<double>> double_series, wp<TS<string>> string_series,
                           wp<TS<symbol>> symbol_series, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto result = frame::with_columns(
            source, "bool_series"_n = bool_series, "bool_constant"_n = true, "i64_series"_n = i64_series,
            "i64_constant"_n = i64{42}, "double_series"_n = double_series, "double_constant"_n = 3.5,
            "string_series"_n = string_series, "string_constant"_n = std::string_view{"fixed"},
            "symbol_series"_n = symbol_series, "symbol_constant"_n = symbol{"constant"});
        return observe(source, result, tick);
    };

    const auto result = run_graph(
        graph, input<TS<i64>>({1, literal::SKIP, 2, 3, 4, 5, 6}),
        input<TS<bool>>({true, false, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, true}),
        input<TS<i64>>({10, 11, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, 12}),
        input<TS<double>>({1.5, 2.5, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, 3.5}),
        input<TS<string>>({std::string_view{"first"}, std::string_view{"second"}, literal::SKIP,
                           literal::SKIP, literal::SKIP, literal::SKIP, std::string_view{"last"}}),
        input<TS<symbol>>({symbol{"first"}, symbol{"second"}, literal::SKIP, literal::SKIP, literal::SKIP,
                           literal::SKIP, symbol{"last"}}),
        input<TS<i64>>({0, 1, 2, 3, 4, 5, 6}));

    REQUIRE(result.equals<i64>({0, 1, 2, 3, 4, 5, 6}));
    REQUIRE(observations.size() == 7);
    const auto expected_names = std::vector<std::string>{
        "bool_series", "bool_constant", "i64_series", "i64_constant", "double_series", "double_constant",
        "string_series", "string_constant", "symbol_series", "symbol_constant"};
    for (const auto &observation : observations)
        if (observation.valid)
            CHECK(observation.names == expected_names);

    CHECK(observations[0].valid);
    REQUIRE(observations[0].rows.size() == 1);
    CHECK(observations[0].rows[0].id == 0);
    CHECK(observations[0].rows[0].bool_series == true);
    CHECK(observations[0].rows[0].bool_constant == true);
    CHECK(observations[0].rows[0].i64_series == 10);
    CHECK(observations[0].rows[0].i64_constant == 42);
    CHECK(observations[0].rows[0].double_series == 1.5);
    CHECK(observations[0].rows[0].double_constant == 3.5);
    CHECK(observations[0].rows[0].string_series == "first");
    CHECK(observations[0].rows[0].string_constant == "fixed");
    CHECK(observations[0].rows[0].symbol_series == symbol{"first"});
    CHECK(observations[0].rows[0].symbol_constant == symbol{"constant"});

    REQUIRE(observations[1].rows.size() == 1);
    CHECK(observations[1].rows[0].bool_series == false);
    CHECK(observations[1].rows[0].i64_series == 11);
    CHECK(observations[1].rows[0].double_series == 2.5);
    CHECK(observations[1].rows[0].string_series == "second");
    CHECK(observations[1].rows[0].symbol_series == symbol{"second"});

    REQUIRE(observations[2].rows.size() == 2);
    CHECK(observations[2].rows[0].id == 0);
    CHECK(observations[2].rows[1].id == 1);
    CHECK(observations[2].rows[1].i64_series == 11);
    CHECK(observations[3].rows.size() == 1);
    CHECK(observations[3].rows[0].id == 1);
    REQUIRE(observations[4].rows.size() == 2);
    CHECK(observations[4].rows[0].id == 0);
    CHECK(observations[4].rows[0].i64_series == 11);
    CHECK(observations[4].rows[0].string_series == "second");
    CHECK(observations[4].rows[1].id == 1);
    CHECK_FALSE(observations[5].valid);
    REQUIRE(observations[6].rows.size() == 1);
    CHECK(observations[6].rows[0].i64_series == 12);
    CHECK(observations[6].rows[0].string_series == "last");
    CHECK(observations[6].rows[0].symbol_series == symbol{"last"});
}

TEST_CASE("with_columns adds named assignments to an anonymous source schema",
          "[feature:frame][feature:with_columns][declaration:generic]") {
    const auto fields = std::array{SchemaField{util::symbol{"base"}, &cpp_scalar_type<i64>()}};
    const auto &schema = anonymous_schema(fields);
    const auto &runtime = frame_time_series_runtime_type<SCHEMA>(
        frame_series_type(schema, TimeSeriesGuarantee::ordinary));
    auto observations = std::vector<RuntimeObservation>{};
    const auto observe = node([&](In<TSF<ResultRuntimeSchema>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = RuntimeObservation{.valid = result.valid()};
        if (result.valid()) {
            for (const auto &field : result.schema().fields())
                observation.names.emplace_back(field.name.text());
            result.for_each_row([&](row_id_t id, std::size_t) {
                observation.runtime_bases.push_back(result.column("base").input<i64>().cell(id));
                observation.runtime_constants.push_back(result.column("constant").input<i64>().cell(id));
                observation.copies.push_back(result.column("series").input<double>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<SCHEMA>> source, wp<TS<double>> series, wp<TS<i64>> tick) {
        const auto result = frame::with_columns(source, "constant"_n = i64{17}, "series"_n = series);
        return observe(result, tick);
    };

    const auto result = run_graph(
        graph,
        input(runtime, {literal::SKIP, {{{"base", i64{5}}}}, {{{"base", i64{7}}}}, literal::SKIP}),
        input<TS<double>>({2.5, literal::SKIP, literal::SKIP, 3.5}), input<TS<i64>>({0, 1, 2, 3}));

    REQUIRE(result.equals<i64>({0, 1, 2, 3}));
    REQUIRE(observations.size() == 4);
    CHECK_FALSE(observations[0].valid);
    CHECK(observations[1].valid);
    CHECK(observations[1].names == std::vector<std::string>{"base", "constant", "series"});
    CHECK(observations[1].runtime_bases == std::vector<std::optional<i64>>{5});
    CHECK(observations[1].runtime_constants == std::vector<std::optional<i64>>{17});
    CHECK(observations[1].copies == std::vector<std::optional<double>>{2.5});
    CHECK(observations[2].runtime_bases == (std::vector<std::optional<i64>>{5, 7}));
    CHECK(observations[2].runtime_constants == (std::vector<std::optional<i64>>{17, 17}));
    CHECK(observations[2].copies == (std::vector<std::optional<double>>{2.5, 2.5}));
    CHECK(observations[3].copies == (std::vector<std::optional<double>>{3.5, 3.5}));
}

TEST_CASE("with_columns does not republish a broadcast column for source cell changes",
          "[feature:frame][feature:with_columns]") {
    auto observations = std::vector<BorrowedBroadcastObservation>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<ResultRuntimeSchema>, optional> result,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        auto observation = BorrowedBroadcastObservation{.valid = result.valid(),
                                                        .frame_modified = result.modified()};
        if (result.valid()) {
            const auto source_price = source.column("price").input<double>().extension_descriptor().storage;
            const auto result_price = result.column("price").input<double>().extension_descriptor().storage;
            observation.pass_through_is_borrowed = source_price.native == result_price.native &&
                                                    source_price.physical_rows == result_price.physical_rows;
            const auto constant = result.column("constant").input<i64>();
            observation.broadcast_modified = constant.modified();
            constant.for_each_changed([&](row_id_t id) { observation.broadcast_changed.push_back(id); });
            result.for_each_row([&](row_id_t id, std::size_t) {
                observation.source_values.push_back(result.column("price").input<double>().cell(id).transform(
                    [](double value) { return static_cast<i64>(value); }));
                observation.broadcast_values.push_back(constant.cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto result = frame::with_columns(source, "constant"_n = i64{5});
        return observe(source, result, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2}), input<TS<i64>>({0, 1}));

    REQUIRE(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].pass_through_is_borrowed);
    CHECK(observations[0].broadcast_changed == std::vector<row_id_t>{0});
    CHECK(observations[0].source_values == std::vector<std::optional<i64>>{8});
    CHECK(observations[0].broadcast_values == std::vector<std::optional<i64>>{5});
    CHECK(observations[1].frame_modified);
    CHECK(observations[1].pass_through_is_borrowed);
    CHECK_FALSE(observations[1].broadcast_modified);
    CHECK(observations[1].broadcast_changed.empty());
    CHECK(observations[1].source_values == std::vector<std::optional<i64>>{12});
    CHECK(observations[1].broadcast_values == std::vector<std::optional<i64>>{5});
}

TEST_CASE("with_columns refills a scalar broadcast when filtered row membership changes",
          "[feature:frame][feature:with_columns][transition:update]") {
    struct Observation {
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<std::optional<i64>> values;
    };
    auto observations = std::vector<Observation>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<ResultRuntimeSchema>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = Observation{};
        if (result.valid()) {
            const auto constant = result.column("constant").input<i64>();
            constant.for_each_changed([&](row_id_t id) { observation.changed.push_back(id); });
            result.for_each_row([&](row_id_t id, std::size_t) {
                observation.rows.push_back(id);
                observation.values.push_back(constant.cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto selected = filter(source, col("price") > 10.0);
        const auto result = frame::with_columns(selected, "constant"_n = i64{5});
        return observe(result, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2, 3, 2}), input<TS<i64>>({0, 1, 2, 3}));

    REQUIRE(result.equals<i64>({0, 1, 2, 3}));
    REQUIRE(observations.size() == 4);
    CHECK(observations[0].rows.empty());
    CHECK(observations[1].rows == std::vector<row_id_t>{0});
    CHECK(observations[1].changed == std::vector<row_id_t>{0});
    CHECK(observations[1].values == std::vector<std::optional<i64>>{5});
    CHECK(observations[2].rows.empty());
    CHECK(observations[3].rows == std::vector<row_id_t>{0});
    CHECK(observations[3].changed == std::vector<row_id_t>{0});
    CHECK(observations[3].values == std::vector<std::optional<i64>>{5});
}

TEST_CASE("with_columns invalidates and refills a TS broadcast input",
          "[feature:frame][feature:with_columns][transition:validity]") {
    struct Observation {
        bool valid{};
        std::vector<std::optional<i64>> values;
    };
    auto observations = std::vector<Observation>{};
    const auto source_node = node<EmptySource>();
    const auto value_node = node<BroadcastValueSource>();
    const auto observe = node([&](In<TSF<ResultRuntimeSchema>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = Observation{.valid = result.valid()};
        if (result.valid()) {
            const auto value = result.column("assigned").input<i64>();
            result.for_each_row([&](row_id_t id, std::size_t) { observation.values.push_back(value.cell(id)); });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto value = value_node(command);
        const auto result = frame::with_columns(source, "assigned"_n = value);
        return observe(result, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2, 3}), input<TS<i64>>({0, 1, 2}));

    REQUIRE(result.equals<i64>({0, 1, 2}));
    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].values == std::vector<std::optional<i64>>{5});
    CHECK_FALSE(observations[1].valid);
    CHECK(observations[2].valid);
    CHECK(observations[2].values == std::vector<std::optional<i64>>{9});
}

TEST_CASE("with_columns reports unsupported scalar and TS cell types as wiring errors",
          "[feature:frame][feature:with_columns][error:wiring]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<Quotes>>) {});
    const auto source = source_node();
    const auto capture_wiring_error = [](auto &&operation) {
        auto error = std::optional<WiringError>{};
        try {
            operation();
        } catch (const WiringError &caught) {
            error = caught;
        }
        return error;
    };
    const auto scalar_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns(
            source, "unsupported_scalar"_n = ScalarValue<List<i64>>{std::array<i64, 1>{1}}));
    });
    REQUIRE(scalar_error);
    CHECK(std::string_view{scalar_error->what()} ==
          "frame::with_columns supports only bool, i64, double, string, or symbol values");

    const auto unsupported_series_node = node([](Out<TS<List<i64>>>) {});
    const auto unsupported_series = unsupported_series_node();
    const auto series_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns(source, "unsupported_series"_n = unsupported_series));
    });
    REQUIRE(series_error);
    CHECK(std::string_view{series_error->what()} ==
          "frame::with_columns supports only bool, i64, double, string, or symbol values");
    static_cast<void>(context);
}

TEST_CASE("with_columns writes the bool batch tail after 128 rows",
          "[feature:frame][feature:with_columns][cell:bool]") {
    const auto source_node = node<ManyRowsSource>();
    auto observed_rows = std::size_t{};
    auto observed_true = std::size_t{};
    const auto observe = node([&](In<TSF<ResultRuntimeSchema>, optional> result, Out<TS<i64>> output) {
        if (result.valid()) {
            const auto flag = result.column("flag").input<bool>();
            result.for_each_row([&](row_id_t id, std::size_t) {
                ++observed_rows;
                observed_true += flag.cell(id).value_or(false);
            });
        }
        output.set(static_cast<i64>(observed_rows));
    });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto source = source_node(command);
        return observe(frame::with_columns(source, "flag"_n = true));
    };

    const auto result = run_graph(graph, input<TS<i64>>({1}));

    CHECK(result.equals<i64>({129}));
    CHECK(observed_rows == 129);
    CHECK(observed_true == 129);
}

} // namespace
