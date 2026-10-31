#include <catch2/catch_test_macros.hpp>

#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/run_graph.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<i64> size;
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field>
struct QuotesWithMid : schema_extends<Quotes, Field> {
    Field<double> mid;
};

template <template <typename, auto...> typename Field> struct KeyedTextQuotes {
    Field<string> id;
    Field<symbol> venue;
    Field<string> description;
    Field<string> text;

    static SchemaMetadata metadata() { return key_metadata<"id", "venue">(); }
};

template <template <typename, auto...> typename Field> struct AssignedTextColumn {
    Field<TSC<string>> text;
};

template <template <typename, auto...> typename Field> struct AssignedKeyColumn {
    Field<TSC<string>> id;
};

struct QuoteSourceState {
    row_id_t row{no_row};
};

struct QuoteSource {
    In<TS<i64>> command;
    State<QuoteSourceState> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->row = output.add(
                Row<Quotes>{.size = 5, .bid = 10.5, .ask = 10.0});
            return;
        case 4:
            output.set(state->row, RowPatch<Quotes>{.size = 3});
            return;
        case 5:
            output.set(state->row, RowPatch<Quotes>{.ask = 11.0});
            return;
        case 6:
            CHECK(output.erase(state->row));
            return;
        default:
            return;
        }
    }
};

struct BindColumns {
    In<TSF<Quotes>> source;
    In<TSB<SCHEMA>> assigned;
    Out<TSF<QuotesWithMid>, view> output;

    void evaluate() const { static_cast<void>(output.bind_columns(source, assigned)); }
};

struct BindColumnsObservation {
    bool valid{};
    bool frame_modified{};
    const void *source_row_domain{};
    const void *output_row_domain{};
    const void *assigned_row_domain{};
    bool shares_column_storage{};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> size_modified;
    std::vector<row_id_t> ask_modified;
    std::vector<row_id_t> mid_modified;
    std::vector<row_id_t> removed;
    std::vector<std::optional<double>> mids;
    std::vector<std::optional<double>> asks;
};

TEST_CASE("TSF view borrows runtime-schema TSB columns and uses current deltas",
          "[feature:frame][transition:update][transition:removal]") {
    auto observations = std::vector<BindColumnsObservation>{};
    const auto assemble = node<BindColumns>();
    const auto observe = node([&](In<TSF<Quotes>> source, In<TSF<QuotesWithMid>, optional> input,
                                  In<TSB<SCHEMA>> assigned, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = BindColumnsObservation{.valid = input.valid(),
                                            .frame_modified = input.modified(),
                                            .source_row_domain = source.row_domain(),
                                            .output_row_domain = input.row_domain()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.mids.push_back(input.cell<3>(id));
            result.asks.push_back(input.cell<2>(id));
        });
        input.for_each_modified<0>([&](row_id_t id) { result.size_modified.push_back(id); });
        input.for_each_modified<2>([&](row_id_t id) { result.ask_modified.push_back(id); });
        input.for_each_modified<3>([&](row_id_t id) { result.mid_modified.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        if (input.valid()) {
            const auto assigned_storage = assigned[1].template as<TSC<double>>().extension_descriptor().storage;
            const auto output_storage = input.column(3).template input<double>().extension_descriptor().storage;
            result.assigned_row_domain = assigned_storage.visible_rows.identity;
            result.shares_column_storage = assigned_storage.native == output_storage.native &&
                                           assigned_storage.physical_rows == output_storage.physical_rows;
        }
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto writer = node<QuoteSource>();
    const auto graph = [&](wp<TS<i64>> command, wp<TS<double>> offset, wp<TS<i64>> tick) {
        const auto source = writer(command);
        const auto mid = bind_expression(source, col("bid") + param("offset"), "offset"_n = offset);
        const auto ask = bind_expression(source, col("bid") + param("offset"), "offset"_n = offset);
        const auto assigned = bundle("ask"_n = ask, "mid"_n = mid);
        return observe(source, assemble(source, assigned), assigned, tick);
    };

    const auto result = run_graph(
        graph, input<TS<i64>>({0, 1, literal::SKIP, 4, 5, 6}),
        input<TS<double>>({0.0, 0.0, 2.0, literal::SKIP, literal::SKIP, literal::SKIP}),
        input<TS<i64>>({0, 1, 2, 3, 4, 5}));

    REQUIRE(result.equals<i64>({0, 1, 2, 3, 4, 5}));
    REQUIRE(observations.size() == 6);
    CHECK(observations[1].valid);
    CHECK(observations[1].source_row_domain == observations[1].output_row_domain);
    CHECK(observations[1].assigned_row_domain == observations[1].source_row_domain);
    CHECK(observations[1].shares_column_storage);
    CHECK(observations[1].rows == std::vector<row_id_t>{0});
    CHECK(observations[1].mids == std::vector<std::optional<double>>{10.5});
    CHECK(observations[1].asks == std::vector<std::optional<double>>{10.5});

    CHECK(observations[2].mid_modified == std::vector<row_id_t>{0});
    CHECK(observations[2].ask_modified == std::vector<row_id_t>{0});
    CHECK(observations[2].size_modified.empty());
    CHECK(observations[2].mids == std::vector<std::optional<double>>{12.5});
    CHECK(observations[2].asks == std::vector<std::optional<double>>{12.5});

    CHECK(observations[3].size_modified == std::vector<row_id_t>{0});
    CHECK(observations[3].mid_modified.empty());
    CHECK(observations[3].ask_modified.empty());
    CHECK(observations[3].mids == std::vector<std::optional<double>>{12.5});
    CHECK(observations[3].asks == std::vector<std::optional<double>>{12.5});

    CHECK(observations[4].ask_modified.empty());
    CHECK_FALSE(observations[4].frame_modified);
    CHECK(observations[4].asks == std::vector<std::optional<double>>{12.5});
    CHECK(observations[5].rows.empty());
    CHECK(observations[5].removed == std::vector<row_id_t>{0});
}

TEST_CASE("TSF view rejects an assigned column from another row domain", "[feature:frame][error:binding]") {
    auto rejected = false;
    const auto writer = node<QuoteSource>();
    const auto try_bind = node([&](In<TSF<Quotes>> source, In<TSB<SCHEMA>> assigned,
                                   Out<TSF<QuotesWithMid>, view> output) {
        try {
            static_cast<void>(output.bind_columns(source, assigned));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
    });
    const auto observe = node([](In<TSF<QuotesWithMid>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        static_cast<void>(input);
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> foreign_command, wp<TS<i64>> tick) {
        const auto source = writer(command);
        const auto foreign_source = writer(foreign_command);
        const auto assigned = bundle("mid"_n = select_column<"bid">(foreign_source));
        return observe(try_bind(source, assigned), tick);
    };

    const auto result = run_graph(graph, std::array<i64, 1>{1}, std::array<i64, 1>{1}, std::array<i64, 1>{0});

    CHECK(result.equals<i64>({0}));
    CHECK(rejected);
}

TEST_CASE("TSF view rejects an assigned column from another selection descriptor",
          "[feature:frame][error:binding]") {
    auto rejected = false;
    const auto writer = node<QuoteSource>();
    const auto try_bind = node([&](In<TSF<Quotes>> source, In<TSB<SCHEMA>> assigned,
                                   Out<TSF<QuotesWithMid>, view> output) {
        try {
            static_cast<void>(output.bind_columns(source, assigned));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
    });
    const auto observe = node([](In<TSF<QuotesWithMid>, optional> input, In<TS<i64>> tick,
                                 Out<TS<i64>> output) {
        static_cast<void>(input);
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = writer(command);
        const auto selected = filter(source, col("size") > i64{0});
        const auto assigned = bundle("mid"_n = select_column<"bid">(selected));
        return observe(try_bind(source, assigned), tick);
    };

    const auto result = run_graph(graph, std::array<i64, 1>{1}, std::array<i64, 1>{0});

    CHECK(result.equals<i64>({0}));
    CHECK(rejected);
}

struct KeyedTextObservation {
    bool valid{};
    bool preserves_key_bindings{};
    bool borrows_assigned_text{};
    std::optional<std::string> id;
    std::optional<symbol> venue;
    std::optional<std::string> description;
    std::optional<std::string> text;
};

TEST_CASE("generic TSF view borrows text and retains keyed fields", "[feature:frame][declaration:generic][cell:text]") {
    auto key_assignment_rejected = false;
    auto observations = std::vector<KeyedTextObservation>{};
    const auto bind = node([&](In<TSF<SCHEMA>> source, In<TSB<AssignedTextColumn>> assigned,
                               In<TSB<AssignedKeyColumn>> assigned_key, Out<TSF<SCHEMA>, view> output) {
        static_cast<void>(output.bind_columns(source, assigned));
        try {
            static_cast<void>(output.bind_columns(source, assigned_key));
        } catch (const std::invalid_argument &) {
            key_assignment_rejected = true;
        }
    });
    const auto observe = node([&](In<TSF<SCHEMA>> source, In<TSF<SCHEMA>, optional> output,
                                  Out<TS<i64>> result) {
        auto observation = KeyedTextObservation{.valid = output.valid()};
        if (output.valid()) {
            const auto source_rows = source.extension_descriptor();
            const auto output_rows = output.extension_descriptor();
            const auto source_id = source_rows.column(source_rows.field_index(util::symbol{"id"}));
            const auto source_venue = source_rows.column(source_rows.field_index(util::symbol{"venue"}));
            const auto source_description = source_rows.column(source_rows.field_index(util::symbol{"description"}));
            const auto source_text = source_rows.column(source_rows.field_index(util::symbol{"text"}));
            const auto output_id = output_rows.column(output_rows.field_index(util::symbol{"id"}));
            const auto output_venue = output_rows.column(output_rows.field_index(util::symbol{"venue"}));
            const auto output_description = output_rows.column(output_rows.field_index(util::symbol{"description"}));
            const auto output_text = output_rows.column(output_rows.field_index(util::symbol{"text"}));
            observation.preserves_key_bindings = source_id.native == output_id.native &&
                                                 source_id.physical_rows == output_id.physical_rows &&
                                                 source_venue.native == output_venue.native &&
                                                 source_venue.physical_rows == output_venue.physical_rows;
            observation.borrows_assigned_text = source_description.native == output_text.native &&
                                                source_description.physical_rows == output_text.physical_rows &&
                                                source_text.native != output_text.native &&
                                                source_text.physical_rows == output_text.physical_rows;
            output.for_each_row([&](row_id_t id, std::size_t) {
                const auto copy_text = [](std::optional<std::string_view> value) -> std::optional<std::string> {
                    return value ? std::optional<std::string>{*value} : std::nullopt;
                };
                observation.id = copy_text(output.column("id").template input<string>().cell(id));
                observation.venue = output.column("venue").template input<symbol>().cell(id);
                observation.description = copy_text(output.column("description").template input<string>().cell(id));
                observation.text = copy_text(output.column("text").template input<string>().cell(id));
            });
        }
        observations.push_back(std::move(observation));
        result.set(static_cast<i64>(output.size()));
    });
    const auto graph = [&](wp<TSF<KeyedTextQuotes>> source) {
        const auto assigned = bundle<TSB<AssignedTextColumn>>("text"_n = select_column<"description">(source));
        const auto assigned_key = bundle<TSB<AssignedKeyColumn>>("id"_n = select_column<"id">(source));
        return observe(source, bind(source, assigned, assigned_key));
    };
    const auto &runtime = frame_time_series_runtime_type<KeyedTextQuotes>(TSF<KeyedTextQuotes>::type());
    const auto result = run_graph(
        graph, input(runtime.type(), {literal::TimeSeriesLiteral{},
                               {{{"id", std::string_view{"row-1"}}, {"venue", symbol{"XNYS"}},
                                 {"description", std::string_view{"borrowed"}},
                                 {"text", std::string_view{"replaced"}}}}}));

    REQUIRE(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[1].valid);
    CHECK(key_assignment_rejected);
    CHECK(observations[1].preserves_key_bindings);
    CHECK(observations[1].borrows_assigned_text);
    CHECK(observations[1].id == std::optional<std::string>{"row-1"});
    CHECK(observations[1].venue == std::optional<symbol>{symbol{"XNYS"}});
    CHECK(observations[1].description == std::optional<std::string>{"borrowed"});
    CHECK(observations[1].text == std::optional<std::string>{"borrowed"});
}

} // namespace
