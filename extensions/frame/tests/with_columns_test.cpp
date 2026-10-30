#include <catch2/catch_test_macros.hpp>

#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/graph.hpp>
#include <bg/operators.hpp>
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

template <template <typename, auto...> typename Field> struct CompositeKeyedQuotes {
    Field<i64> tenant;
    Field<string> id;
    Field<double> price;

    static SchemaMetadata metadata() { return key_metadata<"tenant", "id">(); }
};

template <template <typename, auto...> typename Field> struct KeyedQuotesWithExtraMetadata {
    Field<string> id;
    Field<double> price;

    static SchemaMetadata metadata() {
        const auto marker = ScalarValue<i64>{42};
        return key_metadata<"id">().copy_with(
            make_schema_metadata({schema_metadata_entry("frame.target", marker)}));
    }
};

template <template <typename, auto...> typename Field> struct UnkeyedStringIdTarget {
    Field<string> id;
    Field<double> price;

    static SchemaMetadata metadata() {
        const auto marker = ScalarValue<i64>{24};
        return make_schema_metadata({schema_metadata_entry("frame.target", marker)});
    }
};

template <template <typename, auto...> typename Field> struct ExplicitQuotes {
    Field<double> price;
    Field<double> premium;
    Field<i64> id;

    static SchemaMetadata metadata() {
        const auto marker = ScalarValue<i64>{42};
        return make_schema_metadata({schema_metadata_entry("frame.target", marker)});
    }
};

template <template <typename, auto...> typename Field> struct DuplicatePriceProjection {
    Field<double> best;
    Field<double> reference;
};

template <template <typename, auto...> typename Field> struct DirectOperatorSelection {
    Field<i64> source;
    Field<double> expression;
    Field<double> series;
    Field<double> column;
};

template <template <typename, auto...> typename Field> struct WrongTargetFields {
    Field<double> price;
    Field<double> premium;
    Field<i64> other_id;
};

template <template <typename, auto...> typename Field> struct WrongTargetType {
    Field<i64> id;
    Field<i64> price;
    Field<double> premium;
};

template <template <typename, auto...> typename Field> struct UnkeyedKeyTarget {
    Field<string> id;
    Field<double> price;
    Field<double> premium;
};

template <template <typename, auto...> typename Field> struct ProjectionBase {
    Field<i64> id;
    Field<double> price;
};

template <template <typename, auto...> typename Field>
struct ProjectionDerived : schema_extends<ProjectionBase, Field> {
    Field<double> volume;
};

template <template <typename, auto...> typename Field> struct EmptySchema {};

template <template <typename, auto...> typename Field>
using SourceRuntimeSchema = SchemaVariableDeclaration<"WITH_COLUMNS_TEST_SOURCE_SCHEMA">;
template <template <typename, auto...> typename Field>
using ResultRuntimeSchema = SchemaVariableDeclaration<"WITH_COLUMNS_TEST_RESULT_SCHEMA">;
template <template <typename, auto...> typename Field>
using UnchangedRuntimeSchema = SchemaVariableDeclaration<"WITH_COLUMNS_TEST_UNCHANGED_SCHEMA">;
template <template <typename, auto...> typename Field>
using SelectRuntimeSchema = SchemaVariableDeclaration<"FRAME_SELECT_TEST_RESULT_SCHEMA">;
template <template <typename, auto...> typename Field>
using RenameRuntimeSchema = SchemaVariableDeclaration<"FRAME_RENAME_TEST_RESULT_SCHEMA">;
template <template <typename, auto...> typename Field>
using DropRuntimeSchema = SchemaVariableDeclaration<"FRAME_DROP_TEST_RESULT_SCHEMA">;

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

struct ProjectionSource {
    In<TS<i64>> command;
    Out<TSF<ProjectionDerived>> output;

    void evaluate() const {
        if (*command == 1)
            output.add(ProjectionDerived<Cell>{ProjectionBase<Cell>{Cell<i64>{7}, Cell<double>{8.0}},
                                               Cell<double>{100.0}});
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

struct MixedSelectionObservation {
    bool valid{};
    bool shares_row_domain{};
    bool borrows_price{};
    std::vector<std::optional<i64>> ids;
    std::vector<std::optional<double>> prices;
    std::vector<std::optional<double>> doubled_prices;
};

struct ProjectionObservation {
    bool valid{};
    bool shares_row_domain{};
    bool selected_borrows_columns{};
    bool renamed_borrows_column{};
    bool dropped_borrows_column{};
    std::vector<std::string> selected_names;
    std::vector<std::string> renamed_names;
    std::vector<std::string> dropped_names;
    std::vector<std::optional<i64>> selected_ids;
    std::vector<std::optional<double>> selected_prices;
    std::vector<std::optional<double>> renamed_prices;
    std::vector<std::optional<double>> dropped_prices;
};

struct ExplicitTargetObservation {
    bool same_schema_valid{};
    bool target_valid{};
    bool shares_row_domain{};
    bool has_target_metadata{};
    std::vector<std::string> target_names;
    std::vector<std::optional<double>> same_schema_prices;
    std::vector<std::optional<double>> premiums;
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

TEST_CASE("sample holds a computed frame until an independent signal tick",
          "[feature:frame][feature:with_columns][feature:sampling][series:tsf]") {
    struct observation {
        bool valid{};
        bool modified{};
        bool price_modified{};
        bool computed_modified{};
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> prices;
        std::vector<std::optional<double>> computed;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    const auto observe = node([&](In<TSF<ResultRuntimeSchema>, optional> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        if (input.valid()) {
            const auto price = input.column("price").input<double>();
            const auto computed = input.column("computed").input<double>();
            result.price_modified = price.modified();
            result.computed_modified = computed.modified();
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.prices.push_back(price.cell(id));
                result.computed.push_back(computed.cell(id));
            });
            input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
            input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        }
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto fields = std::array{SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
                                   SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &schema = anonymous_schema(fields, key_metadata<"id">());
    const auto &series_type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    const auto graph = [&](wp<TSF<SCHEMA>> source, wp<TS<i64>> signal, wp<TS<i64>> cycle) {
        const auto computed = frame::with_columns(source, "computed"_n = col("price") + 1.0);
        const auto sampled = operators::sample(signal, computed);
        return observe(sampled, cycle);
    };

    const auto result = run_graph(
        graph,
        input(series_type,
              {{{{"id", i64{7}}, {"price", 10.0}}},
               {literal::REMOVE({{"id", i64{7}}})},
               {{{"id", i64{8}}, {"price", 20.0}}}, literal::SKIP,
               {{{"id", i64{8}}, {"price", 21.0}}}}),
        input<TS<i64>>({0, literal::SKIP, literal::SKIP, 3, literal::SKIP}),
        input<TS<i64>>({0, 1, 2, 3, 4}));

    REQUIRE(result.equals<i64>({0, 1, 2, 3, 4}));
    REQUIRE(observations.size() == 5);
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].prices == std::vector<std::optional<double>>{10.0});
    CHECK(observations[0].computed == std::vector<std::optional<double>>{11.0});
    CHECK(observations[0].price_modified);
    CHECK(observations[0].computed_modified);
    CHECK(observations[0].changed.size() == 1);
    CHECK(observations[1].valid);
    CHECK_FALSE(observations[1].modified);
    CHECK(observations[1].prices == std::vector<std::optional<double>>{10.0});
    CHECK(observations[1].computed == std::vector<std::optional<double>>{11.0});
    CHECK_FALSE(observations[1].price_modified);
    CHECK_FALSE(observations[1].computed_modified);
    CHECK(observations[1].changed.empty());
    CHECK(observations[1].removed.empty());
    CHECK(observations[2].valid);
    CHECK_FALSE(observations[2].modified);
    CHECK(observations[2].prices == std::vector<std::optional<double>>{10.0});
    CHECK(observations[2].computed == std::vector<std::optional<double>>{11.0});
    CHECK_FALSE(observations[2].price_modified);
    CHECK_FALSE(observations[2].computed_modified);
    CHECK(observations[2].changed.empty());
    CHECK(observations[2].removed.empty());
    CHECK(observations[3].valid);
    CHECK(observations[3].modified);
    CHECK(observations[3].prices == std::vector<std::optional<double>>{20.0});
    CHECK(observations[3].computed == std::vector<std::optional<double>>{21.0});
    CHECK(observations[3].price_modified);
    CHECK(observations[3].computed_modified);
    CHECK(observations[3].rows != observations[0].rows);
    CHECK(observations[3].changed == observations[3].rows);
    CHECK(observations[3].removed == observations[0].rows);
    CHECK(observations[4].valid);
    CHECK_FALSE(observations[4].modified);
    CHECK(observations[4].prices == std::vector<std::optional<double>>{20.0});
    CHECK(observations[4].computed == std::vector<std::optional<double>>{21.0});
    CHECK_FALSE(observations[4].price_modified);
    CHECK_FALSE(observations[4].computed_modified);
    CHECK(observations[4].changed.empty());
    CHECK(observations[4].removed.empty());
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
    CHECK(std::string_view{scalar_error->what()}.contains(
        "frame::with_columns supports only bool, i64, double, string, or symbol values"));

    const auto unsupported_series_node = node([](Out<TS<List<i64>>>) {});
    const auto unsupported_series = unsupported_series_node();
    const auto series_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns(source, "unsupported_series"_n = unsupported_series));
    });
    REQUIRE(series_error);
    CHECK(std::string_view{series_error->what()}.contains(
        "frame::with_columns supports only bool, i64, double, string, or symbol values"));
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

TEST_CASE("select combines positional columns, borrowed accessors, and computed expressions",
          "[feature:frame][feature:select]") {
    auto observations = std::vector<MixedSelectionObservation>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<SelectRuntimeSchema>, optional> result,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        auto observation = MixedSelectionObservation{
            .valid = result.valid(),
            .shares_row_domain = result.valid() && source.row_domain() == result.row_domain()};
        if (result.valid()) {
            const auto source_storage = source.extension_descriptor();
            const auto result_storage = result.extension_descriptor();
            observation.borrows_price =
                result_storage.column(1).native == source_storage.column(1).native;
            result.for_each_row([&](row_id_t id, std::size_t) {
                observation.ids.push_back(result.column("id").input<i64>().cell(id));
                observation.prices.push_back(result.column("price_alias").input<double>().cell(id));
                observation.doubled_prices.push_back(result.column("double").input<double>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto result = frame::select(source, "id", "price_alias"_n = source["price"],
                                          "double"_n = col("price") * 2.0);
        return observe(source, result, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2, 3}), input<TS<i64>>({0, 1, 2}));

    REQUIRE(result.equals<i64>({0, 1, 2}));
    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].shares_row_domain);
    CHECK(observations[0].borrows_price);
    CHECK(observations[0].ids == std::vector<std::optional<i64>>{7});
    CHECK(observations[0].prices == std::vector<std::optional<double>>{8.0});
    CHECK(observations[0].doubled_prices == std::vector<std::optional<double>>{16.0});
    CHECK(observations[1].prices == std::vector<std::optional<double>>{12.0});
    CHECK(observations[1].doubled_prices == std::vector<std::optional<double>>{24.0});
    CHECK(observations[2].prices == std::vector<std::optional<double>>{8.0});
    CHECK(observations[2].doubled_prices == std::vector<std::optional<double>>{16.0});
}

TEST_CASE("select operator transports named scalar, expression, TS, and TSC fields",
          "[feature:frame][feature:projection][operator]") {
    auto observed = std::vector<std::array<std::optional<double>, 3>>{};
    auto source_values = std::vector<std::optional<i64>>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<DirectOperatorSelection>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        if (result.valid()) {
            result.for_each_row([&](row_id_t id, std::size_t) {
                source_values.push_back(result.column("source").input<i64>().cell(id));
                observed.push_back({result.column("expression").input<double>().cell(id),
                                    result.column("series").input<double>().cell(id),
                                    result.column("column").input<double>().cell(id)});
            });
        }
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<double>> series, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto selections = bundle("source"_n = i64{19}, "expression"_n = col("price") * 2.0,
                                       "series"_n = series, "column"_n = source["price"]);
        const auto result = operators::select[type<TSF<DirectOperatorSelection>>](source, selections);
        return observe(result, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1}), input<TS<double>>({3.5}), input<TS<i64>>({0}));

    CHECK(result.equals<i64>({0}));
    CHECK(source_values == std::vector<std::optional<i64>>{19});
    CHECK(observed == std::vector<std::array<std::optional<double>, 3>>{{16.0, 3.5, 8.0}});
}

TEST_CASE("select, rename, and drop preserve borrowed columns and row membership",
          "[feature:frame][feature:projection]") {
    auto observations = std::vector<ProjectionObservation>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<SelectRuntimeSchema>, optional> selected,
                                  In<TSF<RenameRuntimeSchema>, optional> renamed,
                                  In<TSF<DropRuntimeSchema>, optional> dropped, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = ProjectionObservation{
            .valid = selected.valid() && renamed.valid() && dropped.valid(),
            .shares_row_domain = selected.valid() && source.row_domain() == selected.row_domain()};
        if (observation.valid) {
            const auto source_storage = source.extension_descriptor();
            const auto selected_storage = selected.extension_descriptor();
            const auto renamed_storage = renamed.extension_descriptor();
            const auto dropped_storage = dropped.extension_descriptor();
            observation.selected_borrows_columns =
                selected_storage.column(0).native == source_storage.column(1).native &&
                selected_storage.column(1).native == source_storage.column(0).native;
            observation.renamed_borrows_column =
                renamed_storage.column(1).native == source_storage.column(1).native;
            observation.dropped_borrows_column =
                dropped_storage.column(0).native == source_storage.column(1).native;
            for (const auto &field : selected.schema().fields())
                observation.selected_names.emplace_back(field.name.text());
            for (const auto &field : renamed.schema().fields())
                observation.renamed_names.emplace_back(field.name.text());
            for (const auto &field : dropped.schema().fields())
                observation.dropped_names.emplace_back(field.name.text());
            selected.for_each_row([&](row_id_t id, std::size_t) {
                observation.selected_prices.push_back(selected.column("price").input<double>().cell(id));
                observation.selected_ids.push_back(selected.column("id").input<i64>().cell(id));
                observation.renamed_prices.push_back(renamed.column("cost").input<double>().cell(id));
                observation.dropped_prices.push_back(dropped.column("price").input<double>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto selected = frame::select(source, {"price", "id"});
        const auto renamed = frame::rename(source, {{"id", "quote_id"}, {"price", "cost"}});
        const auto dropped = frame::drop(source, {"id"});
        return observe(source, selected, renamed, dropped, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2}), input<TS<i64>>({0, 1}));

    REQUIRE(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].shares_row_domain);
    CHECK(observations[0].selected_borrows_columns);
    CHECK(observations[0].renamed_borrows_column);
    CHECK(observations[0].dropped_borrows_column);
    CHECK(observations[0].selected_names == std::vector<std::string>{"price", "id"});
    CHECK(observations[0].renamed_names == std::vector<std::string>{"quote_id", "cost"});
    CHECK(observations[0].dropped_names == std::vector<std::string>{"price"});
    CHECK(observations[0].selected_prices == std::vector<std::optional<double>>{8.0});
    CHECK(observations[0].selected_ids == std::vector<std::optional<i64>>{7});
    CHECK(observations[1].selected_prices == std::vector<std::optional<double>>{12.0});
    CHECK(observations[1].renamed_prices == std::vector<std::optional<double>>{12.0});
    CHECK(observations[1].dropped_prices == std::vector<std::optional<double>>{12.0});
}

TEST_CASE("select accepts runtime names from an anonymous projection of a base annotation",
          "[feature:frame][feature:projection]") {
    auto names = std::vector<std::string>{"volume", "price"};
    auto selected_names = std::vector<std::string>{"price"};
    auto observed = std::vector<std::optional<double>>{};
    const auto source_node = node<ProjectionSource>();
    const auto observe = node([&](In<TSF<SelectRuntimeSchema>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        if (result.valid())
            result.for_each_row([&](row_id_t id, std::size_t) {
                observed.push_back(result.column("price").input<double>().cell(id));
            });
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto derived = source_node(command);
        const auto base = derived.template as<TSF<ProjectionBase>>();
        const auto first = frame::select(base, names);
        const auto selected = frame::select(first, selected_names);
        return observe(selected, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1}), input<TS<i64>>({0}));

    REQUIRE(result.equals<i64>({0}));
    CHECK(observed == std::vector<std::optional<double>>{8.0});
}

TEST_CASE("explicit positional project can rename and repeat a non-key source column",
          "[feature:frame][feature:projection]") {
    auto observed = std::optional<std::array<std::optional<double>, 2>>{};
    auto shares_storage = false;
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<DuplicatePriceProjection>, optional> projected,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        if (projected.valid()) {
            const auto source_column = source.column("price").input<double>().extension_descriptor().storage;
            const auto best_column = projected.column("best").input<double>().extension_descriptor().storage;
            const auto reference_column = projected.column("reference").input<double>().extension_descriptor().storage;
            shares_storage = source_column.native == best_column.native &&
                             source_column.native == reference_column.native;
            projected.for_each_row([&](row_id_t id, std::size_t) {
                observed = std::array{projected.column("best").input<double>().cell(id),
                                      projected.column("reference").input<double>().cell(id)};
            });
        }
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        return observe(source, frame::project<DuplicatePriceProjection, "price", "price">(source), tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1}), input<TS<i64>>({0}));

    REQUIRE(result.equals<i64>({0}));
    REQUIRE(observed);
    CHECK(shares_storage);
    CHECK(*observed == std::array<std::optional<double>, 2>{8.0, 8.0});
}

TEST_CASE("explicit project metadata replaces keyed source metadata",
          "[feature:frame][feature:projection][feature:schema-metadata]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<KeyedQuotesWithExtraMetadata>>) {});
    const auto source = source_node();
    const auto projected = frame::project<UnkeyedStringIdTarget, "id", "price">(source);
    const auto &frame_type = dynamic_cast<const FrameSeriesType &>(projected.type());
    const auto &schema = dynamic_cast<const Schema &>(frame_type.schema_type());

    CHECK(schema.metadata().find("bg.frame.key") == nullptr);
    CHECK(schema.metadata().find("frame.target") != nullptr);
    static_cast<void>(context);
}

TEST_CASE("select and drop preserve unrelated metadata when omitting key columns",
          "[feature:frame][feature:projection][feature:schema-metadata]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<KeyedQuotesWithExtraMetadata>>) {});
    const auto source = source_node();
    const auto retained = frame::select(source, "id", "price");
    const auto selected_without_key = frame::select(source, "price");
    const auto dropped = frame::drop(source, "id");
    const auto dropped_from_anonymous = frame::drop(retained, "id");
    const auto schema_of = [](const auto &point) -> const Schema & {
        const auto &frame_type = dynamic_cast<const FrameSeriesType &>(point.type());
        return dynamic_cast<const Schema &>(frame_type.schema_type());
    };
    const auto marker_value = [](const Schema &schema) -> std::optional<i64> {
        const auto *marker = schema.metadata().find("frame.target");
        if (marker == nullptr)
            return std::nullopt;
        return Scalar<i64>::from(marker->runtime_type(), marker->data()).value();
    };

    const auto &retained_schema = schema_of(retained);
    const auto &selected_schema = schema_of(selected_without_key);
    const auto &dropped_schema = schema_of(dropped);
    const auto &anonymous_schema = schema_of(dropped_from_anonymous);
    CHECK(retained_schema.metadata().find("bg.frame.key") != nullptr);
    CHECK(selected_schema.metadata().find("bg.frame.key") == nullptr);
    CHECK(dropped_schema.metadata().find("bg.frame.key") == nullptr);
    CHECK(anonymous_schema.metadata().find("bg.frame.key") == nullptr);
    CHECK(marker_value(retained_schema) == 42);
    CHECK(marker_value(selected_schema) == 42);
    CHECK(marker_value(dropped_schema) == 42);
    CHECK(marker_value(anonymous_schema) == 42);
    static_cast<void>(context);
}

TEST_CASE("select preserves key metadata for accessor and column expression aliases",
          "[feature:frame][feature:projection]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<KeyedQuotes>>) {});
    const auto source = source_node();
    const auto from_accessor = frame::select(source, "accessor_id"_n = source["id"]);
    const auto from_expression = frame::select(source, "expression_id"_n = col("id"));
    const auto key_names = [](const auto &point) {
        const auto &frame_type = dynamic_cast<const FrameSeriesType &>(point.type());
        const auto &schema = dynamic_cast<const Schema &>(frame_type.schema_type());
        const auto *key = schema.metadata().find("bg.frame.key");
        if (key == nullptr)
            return std::vector<std::string>{};
        const auto values = Scalar<List<string>>::from(key->runtime_type(), key->data());
        auto result = std::vector<std::string>{};
        for (auto index = std::size_t{}; index != values.size(); ++index)
            result.emplace_back(values[index].value());
        return result;
    };

    CHECK(key_names(from_accessor) == std::vector<std::string>{"accessor_id"});
    CHECK(key_names(from_expression) == std::vector<std::string>{"expression_id"});
    static_cast<void>(context);
}

TEST_CASE("select uses the first selected alias for a duplicated key column",
          "[feature:frame][feature:projection]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<KeyedQuotes>>) {});
    const auto source = source_node();
    const auto accessor_first = frame::select(source, "accessor_id"_n = source["id"],
                                              "expression_id"_n = col("id"));
    const auto expression_first = frame::select(source, "expression_id"_n = col("id"),
                                                "accessor_id"_n = source["id"]);
    const auto key_names = [](const auto &point) {
        const auto &frame_type = dynamic_cast<const FrameSeriesType &>(point.type());
        const auto &schema = dynamic_cast<const Schema &>(frame_type.schema_type());
        const auto *key = schema.metadata().find("bg.frame.key");
        if (key == nullptr)
            return std::vector<std::string>{};
        const auto values = Scalar<List<string>>::from(key->runtime_type(), key->data());
        auto result = std::vector<std::string>{};
        for (auto index = std::size_t{}; index != values.size(); ++index)
            result.emplace_back(values[index].value());
        return result;
    };

    CHECK(key_names(accessor_first) == std::vector<std::string>{"accessor_id"});
    CHECK(key_names(expression_first) == std::vector<std::string>{"expression_id"});
    static_cast<void>(context);
}

TEST_CASE("select preserves source key tuple order when output aliases are reordered",
          "[feature:frame][feature:projection]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<CompositeKeyedQuotes>>) {});
    const auto source = source_node();
    const auto selected = frame::select(source, "id_first"_n = source["id"],
                                        "id_second"_n = col("id"),
                                        "tenant_first"_n = col("tenant"),
                                        "tenant_second"_n = source["tenant"]);
    const auto &frame_type = dynamic_cast<const FrameSeriesType &>(selected.type());
    const auto &schema = dynamic_cast<const Schema &>(frame_type.schema_type());
    const auto *key = schema.metadata().find("bg.frame.key");
    REQUIRE(key != nullptr);
    const auto values = Scalar<List<string>>::from(key->runtime_type(), key->data());
    auto key_names = std::vector<std::string>{};
    for (auto index = std::size_t{}; index != values.size(); ++index)
        key_names.emplace_back(values[index].value());

    CHECK(key_names == std::vector<std::string>{"tenant_first", "id_first"});
    static_cast<void>(context);
}

TEST_CASE("select reports missing and duplicate positional names as wiring errors",
          "[feature:frame][feature:projection][error:wiring]") {
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

    const auto missing = capture_wiring_error([&] { static_cast<void>(frame::select(source, "missing")); });
    const auto duplicate = capture_wiring_error([&] { static_cast<void>(frame::select(source, "id", "id")); });

    REQUIRE(missing);
    CHECK(std::string_view{missing->what()}.contains("unknown field"));
    REQUIRE(duplicate);
    CHECK(std::string_view{duplicate->what()}.contains("a schema has duplicate field names"));
    static_cast<void>(context);
}

TEST_CASE("select with no fields preserves the source row domain", "[feature:frame][feature:projection]") {
    auto observed = std::array<bool, 3>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<SelectRuntimeSchema>, optional> selected,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        auto source_rows = std::size_t{};
        auto selected_rows = std::size_t{};
        source.for_each_row([&](row_id_t, std::size_t) { ++source_rows; });
        if (selected.valid())
            selected.for_each_row([&](row_id_t, std::size_t) { ++selected_rows; });
        observed = {selected.valid() && selected.schema().field_count() == 0,
                    selected.valid() && source.row_domain() == selected.row_domain(),
                    source_rows == selected_rows};
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        return observe(source, frame::select(source), tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1}), input<TS<i64>>({0}));

    CHECK(result.equals<i64>({0}));
    CHECK(observed == (std::array<bool, 3>{true, true, true}));
}

TEST_CASE("select does not notify for an unselected source column change",
          "[feature:frame][feature:projection][transition:update]") {
    auto modifications = std::vector<bool>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<SelectRuntimeSchema>, optional> result, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        modifications.push_back(result.modified());
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        return observe(frame::select(source, "id"), tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2}), input<TS<i64>>({0, 1}));

    CHECK(result.equals<i64>({0, 1}));
    CHECK(modifications == std::vector<bool>{true, false});
}

TEST_CASE("with_columns validates explicit target fields, types, keys, and span calls",
          "[feature:frame][feature:with_columns][error:wiring]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<Quotes>>) {});
    const auto source = source_node();
    const auto assignment = std::array<WiringArgument, 1>{WiringArgument{"premium"_n = 2.0}};
    const auto overwrite_assignment = std::array<WiringArgument, 1>{WiringArgument{"price"_n = 2.0}};
    const auto capture_wiring_error = [](auto &&operation) {
        auto error = std::optional<WiringError>{};
        try {
            operation();
        } catch (const WiringError &caught) {
            error = caught;
        }
        return error;
    };

    const auto field_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns<WrongTargetFields>(source, std::span<const WiringArgument>{assignment}));
    });
    const auto type_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns<WrongTargetType>(source, std::span<const WiringArgument>{assignment}));
    });

    REQUIRE(field_error);
    CHECK(std::string_view{field_error->what()}.contains("result schema"));
    REQUIRE(type_error);
    CHECK(std::string_view{type_error->what()}.contains("result schema"));

    const auto same_schema =
        frame::with_columns<Quotes>(source, std::span<const WiringArgument>{overwrite_assignment});
    CHECK(same_schema.type().is_resolved());

    auto keyed_wiring = GraphWiring{};
    const auto keyed_context = WiringContext{keyed_wiring};
    const auto keyed_source_node = node([](Out<TSF<KeyedQuotes>>) {});
    const auto keyed_source = keyed_source_node();
    const auto key_error = capture_wiring_error([&] {
        static_cast<void>(frame::with_columns<UnkeyedKeyTarget>(keyed_source,
                                                                 std::span<const WiringArgument>{assignment}));
    });
    REQUIRE(key_error);
    CHECK(std::string_view{key_error->what()}.contains("ordered key fields"));

    static_cast<void>(keyed_context);
    static_cast<void>(context);
}

TEST_CASE("frame operator rejects a non-frame mapped output", "[feature:frame][feature:projection][error:wiring]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source_node = node([](Out<TSF<Quotes>>) {});
    const auto source = source_node();
    auto error = std::optional<WiringError>{};
    try {
        static_cast<void>(operators::select[type<TS<double>>](source, bundle("id"_n = source["id"])));
    } catch (const WiringError &caught) {
        error = caught;
    }

    REQUIRE(error);
    CHECK(std::string_view{error->what()}.contains("TSF output type"));
    static_cast<void>(context);
}

TEST_CASE("with_columns validates same-schema and reordered explicit targets",
          "[feature:frame][feature:with_columns]") {
    auto observations = std::vector<ExplicitTargetObservation>{};
    const auto source_node = node<QuoteSource>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<Quotes>, optional> same_schema,
                                  In<TSF<ExplicitQuotes>, optional> explicit_target, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto observation = ExplicitTargetObservation{
            .same_schema_valid = same_schema.valid(),
            .target_valid = explicit_target.valid(),
            .shares_row_domain = same_schema.valid() && explicit_target.valid() &&
                                 source.row_domain() == explicit_target.row_domain(),
            .has_target_metadata = explicit_target.valid() &&
                                   explicit_target.schema().metadata().find("frame.target") != nullptr};
        if (same_schema.valid() && explicit_target.valid()) {
            for (const auto &field : explicit_target.schema().fields())
                observation.target_names.emplace_back(field.name.text());
            explicit_target.for_each_row([&](row_id_t id, std::size_t) {
                observation.same_schema_prices.push_back(same_schema.column("price").input<double>().cell(id));
                observation.premiums.push_back(explicit_target.column("premium").input<double>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = source_node(command);
        const auto same_schema = frame::with_columns<Quotes>(source, "price"_n = col("price") * 2.0);
        const auto explicit_target =
            frame::with_columns<ExplicitQuotes>(source, "premium"_n = col("price") * 2.0);
        return observe(source, same_schema, explicit_target, tick);
    };

    const auto result = run_graph(graph, input<TS<i64>>({1, 2}), input<TS<i64>>({0, 1}));

    REQUIRE(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].same_schema_valid);
    CHECK(observations[0].target_valid);
    CHECK(observations[0].shares_row_domain);
    CHECK(observations[0].has_target_metadata);
    CHECK(observations[0].target_names == std::vector<std::string>{"price", "premium", "id"});
    CHECK(observations[0].same_schema_prices == std::vector<std::optional<double>>{16.0});
    CHECK(observations[0].premiums == std::vector<std::optional<double>>{16.0});
    CHECK(observations[1].same_schema_prices == std::vector<std::optional<double>>{24.0});
    CHECK(observations[1].premiums == std::vector<std::optional<double>>{24.0});
}

} // namespace
