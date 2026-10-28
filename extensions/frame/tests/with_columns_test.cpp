#include <catch2/catch_test_macros.hpp>

#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/graph.hpp>
#include <bg/run_graph.hpp>

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

TEST_CASE("with_columns resolves runtime schema and assignment names in run_graph",
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

} // namespace
