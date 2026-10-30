#include <catch2/catch_test_macros.hpp>

#include <bg/feedback.hpp>
#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/run_graph.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct CopyFrame {
    Field<i64> id;
    Field<i64> value;
    Field<i64> other;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field> struct CopyBaseFrame {
    Field<i64> id;
    Field<i64> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field>
struct CopyDerivedFrame : schema_extends<CopyBaseFrame, Field> {
    Field<i64> extra;
};

template <template <typename, auto...> typename Field> struct CopyZeroFieldBase {};

template <template <typename, auto...> typename Field>
struct CopyDerivedFromZeroFieldBase : schema_extends<CopyZeroFieldBase, Field> {
    Field<i64> id;
    Field<i64> extra;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

TEST_CASE("TSC copy_tick forwards generic cell series", "[feature:frame][series:tsc][operation:copy]") {
    const auto copy = node([](In<TSC<SCALAR>> input, Out<TSC<SCALAR>> output) { output.copy_tick(input); });
    const auto graph = [&](wp<TSC<SCALAR>> input) { return copy(input); };

    const auto integer_result = run_graph(
        graph, input<TSC<i64>>({literal::TimeSeriesLiteral{}, {i64{10}, NULL_VALUE}, {i64{20}},
                                literal::INVALID, literal::TimeSeriesLiteral{}, {i64{30}}}));
    CHECK(integer_result.equals({{}, {{"first", i64{10}}, {"second", NULL_VALUE}},
                                 {{"third", i64{20}}}, literal::INVALID, {}, {{"fourth", i64{30}}}}));

    const auto double_result = run_graph(
        graph, input<TSC<double>>({literal::TimeSeriesLiteral{}, {1.5, NULL_VALUE}, {2.5}}));
    CHECK(double_result.equals({{}, {{"first", 1.5}, {"second", NULL_VALUE}}, {{"third", 2.5}}}));

    const auto boolean_result = run_graph(
        graph, input<TSC<bool>>({literal::TimeSeriesLiteral{}, {true, false, NULL_VALUE}, {false, true}}));
    CHECK(boolean_result.equals({{}, {{"first", true}, {"second", false}, {"third", NULL_VALUE}},
                                 {{"fourth", false}, {"fifth", true}}}));

    const auto symbol_result = run_graph(
        graph, input<TSC<symbol>>({literal::TimeSeriesLiteral{}, {symbol{"venue"}, symbol{}, NULL_VALUE},
                                   {symbol{"other"}}}));
    CHECK(symbol_result.equals({{}, {{"first", symbol{"venue"}}, {"second", symbol{}},
                                     {"third", NULL_VALUE}},
                                {{"fourth", symbol{"other"}}}}));

    const auto long_text = std::string(96, 'x');
    const auto long_string_result = run_graph(
        graph, input<TSC<string>>({literal::TimeSeriesLiteral{}, {std::string_view{long_text}, NULL_VALUE},
                                   {std::string_view{long_text}}}));
    CHECK(long_string_result.equals({{}, {{"first", std::string_view{long_text}}, {"second", NULL_VALUE}},
                                     {{"third", std::string_view{long_text}}}}));
}

struct SampledStringSourceState {
    row_id_t row{no_row};
};

struct SampledStringSource {
    In<TS<i64>> cycle;
    State<SampledStringSourceState> state;
    Out<TSC<string>> output;

    void evaluate() const {
        switch (*cycle) {
        case 0:
            state->row = output.add(std::string(96, 'a'));
            output.add(std::nullopt);
            return;
        case 1:
            output.set(state->row, std::string(96, 'b'));
            return;
        case 2:
            static_cast<void>(output.erase(state->row));
            return;
        default:
            return;
        }
    }
};

struct GatedFrameSourceState {
    row_id_t row{no_row};
};

struct GatedFrameSource {
    In<TS<i64>> cycle;
    State<GatedFrameSourceState> state;
    Out<TSF<CopyFrame>> output;

    void evaluate() const {
        switch (*cycle) {
        case 0:
            state->row = output.add(Row<CopyFrame>{.id = 1, .value = 10, .other = 100});
            return;
        case 1:
            output.set(state->row, RowPatch<CopyFrame>{.value = 11});
            return;
        case 3:
            output.set(state->row, RowPatch<CopyFrame>{.other = 101});
            return;
        case 4:
            output.set(state->row, RowPatch<CopyFrame>{.value = 12});
            return;
        default:
            return;
        }
    }
};

struct RetainedFrameTicks {
    RetainedFrameTicks(util::buffer bytes, allocator memory, const TimeSeriesType &type)
        : writer{bytes, memory}, reader{std::move(bytes), type} {}

    TimeSeriesTickWriter writer;
    TimeSeriesTickReader reader;
};

struct ReplayTwoFrameTicks {
    In<TSF<CopyFrame>, optional> input;
    In<TS<i64>> cycle;
    State<RetainedFrameTicks *> ticks;
    Out<TSF<CopyFrame>> output;

    void start(EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        auto bytes = util::buffer::chain(256, memory);
        *ticks = memory.new_object<RetainedFrameTicks>(std::move(bytes), memory, output.runtime_type().type());
    }

    void evaluate(EvaluationContext *context) const {
        auto &retained = **ticks;
        static_cast<void>(retained.writer.write(input, context->evaluation_time()));
        if (*cycle != 2)
            return;
        if (!retained.reader.replay(output) || !retained.reader.replay(output))
            throw std::logic_error{"two retained TSF ticks were not available"};
    }

    void stop(EvaluationContext *context) const noexcept {
        auto memory = context->runtime_graph().allocation();
        memory.delete_object(*ticks);
        *ticks = nullptr;
    }
};

TEST_CASE("sampled string TSC copy retains replaced and removed long strings",
          "[feature:frame][series:tsc][operation:copy][feature:sampling]") {
    struct observation {
        bool valid{};
        bool modified{};
        std::vector<std::optional<std::string>> cells;
    };
    auto observations = std::vector<observation>{};
    const auto source = node<SampledStringSource>();
    const auto observe = node([&](In<TSC<string>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            const auto cell = input.cell(id);
            if (cell)
                result.cells.emplace_back(*cell);
            else
                result.cells.emplace_back(std::nullopt);
        });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle, wp<TS<i64>> signal) {
        return observe(operators::sample(signal, source(cycle)), cycle);
    };

    const auto result = run_graph(graph, input<TS<i64>>({0, 1, 2, 3}),
                                  input<TS<i64>>({0, literal::SKIP, literal::SKIP, 3}));

    CHECK(result.equals<i64>({0, 1, 2, 3}));
    REQUIRE(observations.size() == 4);
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].cells == std::vector<std::optional<std::string>>{std::string(96, 'a'), std::nullopt});
    CHECK(observations[1].valid);
    CHECK_FALSE(observations[1].modified);
    CHECK(observations[1].cells == observations[0].cells);
    CHECK(observations[2].valid);
    CHECK_FALSE(observations[2].modified);
    CHECK(observations[2].cells == observations[0].cells);
    CHECK(observations[3].valid);
    CHECK(observations[3].modified);
    CHECK(observations[3].cells == std::vector<std::optional<std::string>>{std::nullopt});
}

TEST_CASE("feedback retains replaced and removed long TSC strings",
          "[feature:frame][series:tsc][feature:feedback][transition:update][transition:removal]") {
    struct observation {
        bool valid{};
        std::vector<std::optional<std::string>> cells;
    };
    auto observations = std::vector<observation>{};
    const auto source = node<SampledStringSource>();
    const auto observe = node([&](In<TSC<string>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            const auto cell = input.cell(id);
            if (cell)
                result.cells.emplace_back(*cell);
            else
                result.cells.emplace_back(std::nullopt);
        });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        auto retained = feedback<TSC<string>>("strings");
        retained = source(cycle);
        return observe(retained, cycle);
    };

    const auto result = run_graph(graph, std::array<i64, 4>{0, 1, 2, 3});

    CHECK(result.equals<i64>({0, 1, 2, 3}));
    REQUIRE(observations.size() == 4);
    CHECK_FALSE(observations[0].valid);
    CHECK(observations[1].valid);
    CHECK(observations[1].cells ==
          std::vector<std::optional<std::string>>{std::string(96, 'a'), std::nullopt});
    CHECK(observations[2].valid);
    CHECK(observations[2].cells ==
          std::vector<std::optional<std::string>>{std::string(96, 'b'), std::nullopt});
    CHECK(observations[3].valid);
    CHECK(observations[3].cells == std::vector<std::optional<std::string>>{std::nullopt});
}

TEST_CASE("gate preserves TSF row mapping across replay, live copy, and replay",
          "[feature:frame][series:tsf][feature:operator][operation:copy]") {
    struct observation {
        bool valid{};
        bool modified{};
        std::array<bool, 3> columns{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<std::array<std::optional<i64>, 3>> values;
    };
    auto observations = std::vector<observation>{};
    const auto source = node<GatedFrameSource>();
    const auto condition = node([](In<TS<i64>> cycle, Out<TS<bool>> output) {
        output.set(*cycle != 0 && *cycle != 4);
    });
    const auto observe = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified(),
                                  .columns = {input.column(0).modified(), input.column(1).modified(),
                                              input.column(2).modified()}};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back({input.cell<0>(id), input.cell<1>(id), input.cell<2>(id)});
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        return observe(operators::gate(condition(cycle), source(cycle)), cycle);
    };

    const auto result = run_graph(graph, std::array<i64, 6>{0, 1, 2, 3, 4, 5});

    CHECK(result.equals<i64>({0, 1, 2, 3, 4, 5}));
    REQUIRE(observations.size() == 6);
    CHECK_FALSE(observations[0].valid);
    CHECK(observations[1].valid);
    CHECK(observations[1].modified);
    CHECK((observations[1].columns == std::array{true, true, true}));
    REQUIRE(observations[1].rows.size() == 1);
    CHECK((observations[1].values.front() == std::array<std::optional<i64>, 3>{1, 10, 100}));
    CHECK(observations[2].modified);
    CHECK((observations[2].columns == std::array{false, true, false}));
    REQUIRE(observations[2].rows.size() == 1);
    CHECK((observations[2].values.front() == std::array<std::optional<i64>, 3>{1, 11, 100}));
    CHECK(observations[3].modified);
    CHECK((observations[3].columns == std::array{false, false, true}));
    REQUIRE(observations[3].rows.size() == 1);
    CHECK((observations[3].values.front() == std::array<std::optional<i64>, 3>{1, 11, 101}));
    CHECK_FALSE(observations[4].modified);
    CHECK((observations[4].columns == std::array{false, false, false}));
    REQUIRE(observations[4].rows.size() == 1);
    CHECK((observations[4].values.front() == std::array<std::optional<i64>, 3>{1, 11, 101}));
    CHECK(observations[5].modified);
    CHECK((observations[5].columns == std::array{false, true, false}));
    REQUIRE(observations[5].rows.size() == 1);
    CHECK((observations[5].values.front() == std::array<std::optional<i64>, 3>{1, 12, 101}));
}

TEST_CASE("TSF child changes accumulate when one evaluation replays multiple records",
          "[feature:frame][series:tsf][feature:tick-replay]") {
    struct observation {
        bool valid{};
        bool modified{};
        std::array<bool, 3> columns{};
        std::vector<row_id_t> rows;
        std::vector<std::array<std::optional<i64>, 3>> values;
    };
    auto observations = std::vector<observation>{};
    const auto source = node<GatedFrameSource>();
    const auto replay = node<ReplayTwoFrameTicks>();
    const auto observe = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified(),
                                  .columns = {input.column(0).modified(), input.column(1).modified(),
                                              input.column(2).modified()}};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back({input.cell<0>(id), input.cell<1>(id), input.cell<2>(id)});
        });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(replay(source(cycle), cycle), cycle); };

    const auto result = run_graph(graph, std::array<i64, 3>{0, 1, 2});

    CHECK(result.equals<i64>({0, 1, 2}));
    REQUIRE(observations.size() == 3);
    CHECK_FALSE(observations[0].valid);
    CHECK_FALSE(observations[1].valid);
    CHECK(observations[2].valid);
    CHECK(observations[2].modified);
    CHECK((observations[2].columns == std::array{true, true, true}));
    REQUIRE(observations[2].rows.size() == 1);
    CHECK((observations[2].values.front() == std::array<std::optional<i64>, 3>{1, 11, 100}));
}

TEST_CASE("TSF copy_tick forwards row and field deltas", "[feature:frame][series:tsf][operation:copy]") {
    struct observation {
        bool valid{};
        bool modified{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<row_id_t> value_modified;
        std::vector<row_id_t> other_modified;
        std::vector<std::array<std::optional<i64>, 3>> values;
    };
    auto observations = std::vector<observation>{};
    auto copy_cycles = std::vector<i64>{};
    const auto copy = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle,
                              Out<TSF<CopyFrame>> output) {
        copy_cycles.push_back(*cycle);
        output.copy_tick(input);
    });
    const auto observe = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back({input.cell<0>(id), input.cell<1>(id), input.cell<2>(id)});
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        input.for_each_modified<1>([&](row_id_t id) { result.value_modified.push_back(id); });
        input.for_each_modified<2>([&](row_id_t id) { result.other_modified.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TSF<CopyFrame>> input, wp<TS<i64>> cycle) {
        return observe(copy(input, cycle), cycle);
    };

    const auto updates = input<TSF<CopyFrame>>(
        {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"value", i64{10}}, {"other", i64{100}}}},
         {{{"id", i64{1}}, {"value", i64{11}}}}, literal::SKIP,
         {literal::REMOVE({{"id", i64{1}}})},
         {{{"id", i64{2}}, {"value", i64{20}}, {"other", i64{200}}}}, literal::INVALID,
         literal::TimeSeriesLiteral{}, {{{"id", i64{3}}, {"value", i64{30}}, {"other", i64{300}}}}});
    const auto cycles = std::array<i64, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto result = run_graph(graph, updates, cycles);

    CHECK(result.equals<i64>({0, 1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(copy_cycles == std::vector<i64>{0, 1, 2, 3, 4, 5, 6, 7, 8});
    REQUIRE(observations.size() == cycles.size());
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].rows.empty());
    REQUIRE(observations[1].rows.size() == 1);
    CHECK((observations[1].values.front() == std::array<std::optional<i64>, 3>{1, 10, 100}));
    CHECK(observations[2].changed == observations[1].rows);
    CHECK(observations[2].value_modified == observations[1].rows);
    CHECK(observations[2].other_modified.empty());
    CHECK((observations[2].values.front() == std::array<std::optional<i64>, 3>{1, 11, 100}));
    CHECK_FALSE(observations[3].modified);
    CHECK(observations[3].changed.empty());
    CHECK(observations[3].removed.empty());
    CHECK(observations[3].values == observations[2].values);
    CHECK(observations[4].rows.empty());
    CHECK(observations[4].removed == observations[1].rows);
    REQUIRE(observations[5].rows.size() == 1);
    CHECK((observations[5].values.front() == std::array<std::optional<i64>, 3>{2, 20, 200}));
    CHECK_FALSE(observations[6].valid);
    CHECK(observations[6].rows.empty());
    CHECK(observations[7].valid);
    CHECK(observations[7].modified);
    CHECK(observations[7].rows.empty());
    REQUIRE(observations[8].rows.size() == 1);
    CHECK((observations[8].values.front() == std::array<std::optional<i64>, 3>{3, 30, 300}));
}

TEST_CASE("TSF copy_tick copies a declared derived schema into its Base",
          "[feature:frame][series:tsf][operation:copy]") {
    struct observation {
        bool modified{};
        std::array<bool, 2> columns{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<std::array<std::optional<i64>, 2>> values;
    };
    auto observations = std::vector<observation>{};
    const auto copy = node([](In<TSF<CopyDerivedFrame>> input, Out<TSF<CopyBaseFrame>> output) {
        output.copy_tick(input);
    });
    const auto observe = node([&](In<TSF<CopyBaseFrame>, optional> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = observation{.modified = input.modified(),
                                  .columns = {input.column(0).modified(), input.column(1).modified()}};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back({input.cell<0>(id), input.cell<1>(id)});
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TSF<CopyDerivedFrame>> input, wp<TS<i64>> cycle) {
        return observe(copy(input), cycle);
    };

    const auto source = input<TSF<CopyDerivedFrame>>(
        {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"value", i64{10}}, {"extra", i64{100}}}},
         {{{"id", i64{1}}, {"extra", i64{101}}}}, {{{"id", i64{1}}, {"value", i64{11}}}}});
    const auto cycles = std::array<i64, 4>{0, 1, 2, 3};
    const auto result = run_graph(graph, source, cycles);

    CHECK(result.equals<i64>({0, 1, 2, 3}));
    REQUIRE(observations.size() == cycles.size());
    CHECK(observations[1].modified);
    REQUIRE(observations[1].rows.size() == 1);
    CHECK((observations[1].values.front() == std::array<std::optional<i64>, 2>{1, 10}));
    CHECK(observations[2].modified);
    CHECK((observations[2].columns == std::array{false, false}));
    CHECK(observations[2].changed.empty());
    CHECK(observations[2].values == observations[1].values);
    CHECK(observations[3].modified);
    CHECK((observations[3].columns == std::array{false, true}));
    CHECK(observations[3].changed == observations[1].rows);
    CHECK((observations[3].values.front() == std::array<std::optional<i64>, 2>{1, 11}));
}

TEST_CASE("TSF copy_tick copies runtime-schema inheritance into a declared Base",
          "[feature:frame][series:tsf][operation:copy]") {
    const auto &base_schema = static_cast<const Schema &>(frame_schema_type<CopyBaseFrame>());
    const auto derived_fields = std::array{SchemaField{util::symbol{"extra"}, &cpp_scalar_type<i64>()}};
    const auto &derived_schema = schema(util::symbol{"RuntimeCopyDerived"}, new_runtime_schema_id(), &base_schema,
                                        derived_fields);
    const auto &runtime = frame_time_series_runtime_type<SCHEMA>(
        frame_series_type(derived_schema, TimeSeriesGuarantee::ordinary));

    struct observation {
        bool modified{};
        std::vector<row_id_t> rows;
        std::vector<std::array<std::optional<i64>, 2>> values;
    };
    auto observations = std::vector<observation>{};
    const auto copy = node([](In<TSF<CopyBaseFrame>> input, Out<TSF<CopyBaseFrame>> output) {
        output.copy_tick(input);
    });
    const auto observe = node([&](In<TSF<CopyBaseFrame>, optional> input, Out<TS<i64>> output) {
        auto result = observation{.modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back({input.cell<0>(id), input.cell<1>(id)});
        });
        observations.push_back(std::move(result));
        output.set(static_cast<i64>(observations.size() - 1));
    });
    const auto graph = [&](wp<TSF<CopyBaseFrame>> input) { return observe(copy(input)); };
    const auto result = run_graph(
        graph, bg::input(runtime.type(), {literal::TimeSeriesLiteral{},
                                   {{{"id", i64{7}}, {"value", i64{70}}, {"extra", i64{700}}}}}));

    CHECK(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].modified);
    CHECK(observations[0].rows.empty());
    REQUIRE(observations[1].rows.size() == 1);
    CHECK((observations[1].values.front() == std::array<std::optional<i64>, 2>{7, 70}));
}

TEST_CASE("TSF copy_tick preserves row membership for a zero-field Base",
          "[feature:frame][series:tsf][operation:copy]") {
    struct observation {
        bool modified{};
        std::size_t size{};
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    const auto copy = node([](In<TSF<CopyDerivedFromZeroFieldBase>> input,
                              Out<TSF<CopyZeroFieldBase>> output) { output.copy_tick(input); });
    const auto observe = node([&](In<TSF<CopyZeroFieldBase>, optional> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto result = observation{.modified = input.modified(), .size = input.size()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TSF<CopyDerivedFromZeroFieldBase>> input, wp<TS<i64>> cycle) {
        return observe(copy(input), cycle);
    };
    const auto source = input<TSF<CopyDerivedFromZeroFieldBase>>(
        {literal::TimeSeriesLiteral{}, {{{"id", i64{3}}, {"extra", i64{30}}}},
         {{{"id", i64{3}}, {"extra", i64{31}}}}, {literal::REMOVE({{"id", i64{3}}})}});
    const auto cycles = std::array<i64, 4>{0, 1, 2, 3};
    const auto result = run_graph(graph, source, cycles);

    CHECK(result.equals<i64>({0, 1, 2, 3}));
    REQUIRE(observations.size() == cycles.size());
    CHECK(observations[1].modified);
    CHECK(observations[1].size == 1);
    CHECK(observations[1].changed.size() == 1);
    CHECK(observations[2].modified);
    CHECK(observations[2].size == 1);
    CHECK(observations[2].changed.empty());
    CHECK(observations[2].removed.empty());
    CHECK(observations[3].modified);
    CHECK(observations[3].size == 0);
    CHECK(observations[3].removed == observations[1].changed);
}

TEST_CASE("copy_tick publishes a TSC tick when row deltas cancel", "[feature:frame][series:tsc][operation:copy]") {
    struct observation {
        bool valid{};
        bool modified{};
        bool provider_matches{};
        std::size_t size{};
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    auto output_provider_matches = false;
    const auto write = node([](In<TS<i64>> cycle, Out<TSC<i64>> output) {
        if (*cycle == 0) {
            output.clear();
        } else if (*cycle == 1) {
            const auto id = output.add(42);
            static_cast<void>(output.erase(id));
        }
    });
    const auto copy = node([&](In<TSC<i64>, optional> input, In<TS<i64>> cycle, Out<TSC<i64>> output) {
        output.visit([&](const Out<TS_T> *provider) {
            output_provider_matches = &provider->runtime_type() == &output.runtime_type();
        }, ddv::noop);
        output.copy_tick(input);
    });
    const auto observe = node([&](In<TSC<i64>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.visit([&](const In<TS_T> *provider) {
            result.provider_matches = &provider->runtime_type() == &input.runtime_type();
        }, ddv::noop);
        result.size = input.size();
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(copy(write(cycle), cycle), cycle); };
    const auto result = run_graph(graph, std::array<i64, 3>{0, 1, 2});

    CHECK(result.equals<i64>({0, 1, 2}));
    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].provider_matches);
    CHECK(output_provider_matches);
    CHECK(observations[0].size == 0);
    CHECK(observations[1].modified);
    CHECK(observations[1].size == 0);
    CHECK(observations[1].changed.empty());
    CHECK(observations[1].removed.empty());
    CHECK_FALSE(observations[2].modified);
}

TEST_CASE("copy_tick publishes TSF child ticks when row deltas cancel",
          "[feature:frame][series:tsf][operation:copy]") {
    struct observation {
        bool valid{};
        bool modified{};
        bool provider_matches{};
        std::array<bool, 3> columns{};
        std::size_t size{};
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    auto output_provider_matches = false;
    const auto write = node([](In<TS<i64>> cycle, Out<TSF<CopyFrame>> output) {
        if (*cycle == 0) {
            output.clear();
        } else if (*cycle == 1) {
            const auto id = output.add(Row<CopyFrame>{.id = i64{1}, .value = i64{10}, .other = i64{100}});
            static_cast<void>(output.erase(id));
        }
    });
    const auto copy = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle,
                               Out<TSF<CopyFrame>> output) {
        output.visit([&](const Out<TS_T> *provider) {
            output_provider_matches = &provider->runtime_type() == &output.runtime_type();
        }, ddv::noop);
        output.copy_tick(input);
    });
    const auto observe = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(),
                                  .modified = input.modified(),
                                  .provider_matches = false,
                                  .columns = {input.column(0).modified(), input.column(1).modified(),
                                              input.column(2).modified()},
                                  .size = input.size()};
        input.visit([&](const In<TS_T> *provider) {
            result.provider_matches = &provider->runtime_type() == &input.runtime_type();
        }, ddv::noop);
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(copy(write(cycle), cycle), cycle); };
    const auto result = run_graph(graph, std::array<i64, 3>{0, 1, 2});

    CHECK(result.equals<i64>({0, 1, 2}));
    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].provider_matches);
    CHECK(output_provider_matches);
    CHECK((observations[0].columns == std::array{true, true, true}));
    CHECK(observations[1].modified);
    CHECK((observations[1].columns == std::array{true, true, true}));
    CHECK(observations[1].size == 0);
    CHECK(observations[1].changed.empty());
    CHECK(observations[1].removed.empty());
    CHECK_FALSE(observations[2].modified);
    CHECK((observations[2].columns == std::array{false, false, false}));
}

TEST_CASE("full TSF copy republishes child ticks for an already valid empty frame",
          "[feature:frame][series:tsf][operation:copy]") {
    auto observations = std::vector<std::array<bool, 4>>{};
    const auto copy = node([](In<TSF<CopyFrame>> input, In<TS<i64>> cycle, Out<TSF<CopyFrame>> output) {
        output.copy(input);
    });
    const auto observe = node([&](In<TSF<CopyFrame>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        observations.push_back({input.valid(), input.modified(), input.column(0).modified(),
                                input.column(1).modified()});
        output.set(*cycle);
    });
    const auto graph = [&](wp<TSF<CopyFrame>> input, wp<TS<i64>> cycle) { return observe(copy(input, cycle), cycle); };
    const auto result = run_graph(graph, input<TSF<CopyFrame>>({literal::TimeSeriesLiteral{}}),
                                  std::array<i64, 2>{0, 1});

    CHECK(result.equals<i64>({0, 1}));
    REQUIRE(observations.size() == 2);
    CHECK((observations[0] == std::array{true, true, true, true}));
    CHECK((observations[1] == std::array{true, true, true, true}));
}

template <template <typename, auto...> typename Field> struct EmptyCopyFrame {};

TEST_CASE("TSF copy_tick forwards a nonempty zero-column frame", "[feature:frame][series:tsf][operation:copy]") {
    auto valid = false;
    auto modified = false;
    auto size = std::size_t{};
    const auto write = node([](In<TS<i64>> command, Out<TSF<EmptyCopyFrame>> output) {
        if (*command == 0)
            static_cast<void>(output.add(Row<EmptyCopyFrame>{}));
    });
    const auto copy = node([](In<TSF<EmptyCopyFrame>> input, Out<TSF<EmptyCopyFrame>> output) {
        output.copy_tick(input);
    });
    const auto observe = node([&](In<TSF<EmptyCopyFrame>, optional> input, Out<TS<i64>> output) {
        valid = input.valid();
        modified = input.modified();
        size = input.size();
        output.set(0);
    });
    const auto graph = [&](wp<TS<i64>> command) { return observe(copy(write(command))); };
    const auto result = run_graph(graph, std::array<i64, 1>{0});

    CHECK(result.equals<i64>({0}));
    CHECK(valid);
    CHECK(modified);
    CHECK(size == 1);
}

TEST_CASE("delay replays ordinary frame column and frame values",
          "[feature:frame][feature:timed-scheduling][series:value][operation:delay]") {
    const auto delay_column = [](wp<TS<Column<i64>>> input) {
        return operators::delay(input, timedelta_t{1});
    };
    const auto column_result = run_graph(
        delay_column, input<TS<Column<i64>>>({{i64{1}, i64{2}}, {i64{3}}}));
    CHECK(column_result.equals<Column<i64>>({std::nullopt, Column<i64>{{i64{1}, i64{2}}}, Column<i64>{{i64{3}}}}));

    const auto delay_frame = [](wp<TS<Frame<CopyFrame>>> input) {
        return operators::delay(input, timedelta_t{1});
    };
    const auto frame_result = run_graph(
        delay_frame,
        input<TS<Frame<CopyFrame>>>({{{"id", {i64{1}, i64{2}}}, {"value", {i64{10}, i64{20}}},
                                      {"other", {i64{100}, i64{200}}}},
                                     {{"id", {i64{3}}}, {"value", {i64{30}}}, {"other", {i64{300}}}}}));
    const auto first_frame = ScalarValue<Frame<CopyFrame>>::from_literal(
        {{"id", {i64{1}, i64{2}}}, {"value", {i64{10}, i64{20}}}, {"other", {i64{100}, i64{200}}}});
    const auto second_frame = ScalarValue<Frame<CopyFrame>>::from_literal(
        {{"id", {i64{3}}}, {"value", {i64{30}}}, {"other", {i64{300}}}});
    CHECK(frame_result.equals<Frame<CopyFrame>>(
        {std::nullopt, first_frame.view().value(), second_frame.view().value()}));
}

} // namespace
