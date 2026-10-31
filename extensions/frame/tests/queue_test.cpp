#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>

#include <bg/bg.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/queue.hpp>
#include <bg/queue_node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bg;
using namespace bg::frame;

namespace {

template <template <typename, auto...> typename Field> struct QueueRows {
    Field<i64> id;
    Field<string> text;
    Field<double> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field> struct PlainQueueRows {
    Field<string> text;
    Field<double> value;
};

template <template <typename, auto...> typename Field> struct QueueColumns {
    Field<TSC<i64>> integers;
    Field<TSC<string>> strings;
};

template <template <typename, auto...> typename Field> struct EmptyQueueRows {};

EvaluationTime time(i64 microseconds) { return MIN_DT + std::chrono::microseconds{microseconds}; }

struct QueueRow {
    i64 id{};
    std::optional<std::string> text;
    std::optional<double> value;
    friend bool operator==(const QueueRow &, const QueueRow &) = default;
};

struct RuntimeQueueSource {
    static constexpr bool uses_queue = true;

    explicit RuntimeQueueSource(const TimeSeriesRuntimeType &runtime_type)
        : runtime_type_{&runtime_type},
          signature_{util::symbol{"frame_queue_runtime"}, {}, runtime_type.type()} {}

    const Signature &signature() const noexcept { return signature_; }
    bool uses_output_runtime_type() const noexcept { return true; }
    const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &) const noexcept { return *runtime_type_; }
    QueueMode queue_mode() const noexcept { return QueueMode::standard; }

    static void start(NodeInvocation<>) {}
    static void evaluate(NodeInvocation<>) {}

  private:
    const TimeSeriesRuntimeType *runtime_type_;
    Signature signature_;
};

class CountingResource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t deallocations{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

} // namespace

TEST_CASE("keyed TSF queues apply row changes and retry a same-key conflict next cycle",
          "[feature:frame][series:tsf][queue][transition:update][transition:removal][failure]") {
    auto snapshots = std::vector<std::optional<std::vector<QueueRow>>>{};
    const auto source = queue<TSF<QueueRows>>(start = [](Queue) {}, mode = QueueMode::standard);
    const auto observe = node([&](In<TSF<QueueRows>, optional> input) {
        if (!input.valid()) {
            snapshots.emplace_back(std::nullopt);
            return;
        }

        auto rows = std::vector<QueueRow>{};
        const auto ids = input.column("id").input<i64>();
        const auto text = input.column("text").input<string>();
        const auto values = input.column("value").input<double>();
        input.for_each_row([&](row_id_t id, std::size_t) {
            const auto text_cell = text.cell(id);
            rows.push_back({*ids.cell(id), text_cell ? std::optional<std::string>{*text_cell} : std::nullopt,
                            values.cell(id)});
        });
        std::ranges::sort(rows, {}, &QueueRow::id);
        snapshots.emplace_back(std::move(rows));
    });

    auto wiring = GraphWiring{};
    WiringPoint output;
    {
        const auto context = WiringContext{wiring};
        output = source();
        observe(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(literal::TimeSeriesLiteral{literal::REMOVE({{"id", i64{9}}})}));
    runtime.evaluate(time(2));
    CHECK(snapshots.empty());

    auto producer_text = std::string(256, 'q');
    const auto long_text = producer_text;
    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{1}}, {"text", std::string_view{producer_text}},
                                                    {"value", 1.5}},
                                                   {{"id", i64{2}}, {"text", "second"}, {"value", 2.5}}}));
    producer_text.assign(256, 'x');
    runtime.evaluate(time(3));
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back() == std::vector<QueueRow>{{1, long_text, 1.5}, {2, "second", 2.5}});

    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{1}}, {"value", 3.5}},
                                                   literal::REMOVE({{"id", i64{2}}}),
                                                   {{"id", i64{2}}, {"text", "replacement"}, {"value", 4.5}}}));
    runtime.evaluate(time(4));
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back() == std::vector<QueueRow>{{1, long_text, 3.5}, {2, "replacement", 4.5}});

    REQUIRE(queue.write(literal::TimeSeriesLiteral{literal::REMOVE({{"id", i64{9}}})}));
    runtime.evaluate(time(5));
    CHECK(snapshots.size() == 2);

    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{1}}, {"value", 5.5}}}));
    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{1}}, {"value", 6.5}}}));
    runtime.evaluate(time(6));
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots.back()->front().value == 5.5);
    runtime.evaluate(time(7));
    REQUIRE(snapshots.size() == 4);
    CHECK(snapshots.back()->front().value == 6.5);

    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{1}}, {"value", 7.5}}}));
    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{9}}, {"text", "independent"}, {"value", 9.5}}}));
    runtime.evaluate(time(8));
    REQUIRE(snapshots.size() == 5);
    CHECK(snapshots.back() == std::vector<QueueRow>{{1, long_text, 7.5}, {2, "replacement", 4.5},
                                                     {9, "independent", 9.5}});

    REQUIRE(queue.write(literal::TimeSeriesLiteral(literal::INVALID)));
    runtime.evaluate(time(9));
    REQUIRE(snapshots.size() == 6);
    CHECK_FALSE(snapshots.back().has_value());
    REQUIRE(queue.write(literal::TimeSeriesLiteral{}));
    runtime.evaluate(time(10));
    REQUIRE(snapshots.size() == 7);
    CHECK(snapshots.back() == std::vector<QueueRow>{});
    runtime.stop();
}

TEST_CASE("keyed TSF queues retry a reinsert after a canceled add and ignore key-only updates",
          "[feature:frame][series:tsf][queue][transition:addition][transition:removal][transition:update]") {
    auto snapshots = std::vector<std::vector<QueueRow>>{};
    const auto source = queue<TSF<QueueRows>>(start = [](Queue) {}, mode = QueueMode::standard);
    const auto observe = node([&](In<TSF<QueueRows>> input) {
        auto rows = std::vector<QueueRow>{};
        const auto ids = input.column("id").input<i64>();
        const auto text = input.column("text").input<string>();
        const auto values = input.column("value").input<double>();
        input.for_each_row([&](row_id_t id, std::size_t) {
            const auto text_cell = text.cell(id);
            rows.push_back({*ids.cell(id), text_cell ? std::optional<std::string>{*text_cell} : std::nullopt,
                            values.cell(id)});
        });
        std::ranges::sort(rows, {}, &QueueRow::id);
        snapshots.push_back(std::move(rows));
    });

    auto wiring = GraphWiring{};
    WiringPoint output;
    {
        const auto context = WiringContext{wiring};
        output = source();
        observe(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{1}}, {"text", "base"}, {"value", 1.0}}}));
    runtime.evaluate(time(2));
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back() == std::vector<QueueRow>{{1, "base", 1.0}});

    REQUIRE(queue.write(literal::TimeSeriesLiteral{
        {{"id", i64{2}}, {"text", "canceled"}, {"value", 2.0}}, literal::REMOVE({{"id", i64{2}}})}));
    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{2}}, {"text", "reinserted"}, {"value", 3.0}}}));
    runtime.evaluate(time(3));
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back() == std::vector<QueueRow>{{1, "base", 1.0}});
    runtime.evaluate(time(4));
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots.back() == std::vector<QueueRow>{{1, "base", 1.0}, {2, "reinserted", 3.0}});

    REQUIRE(queue.write(literal::TimeSeriesLiteral{{{"id", i64{2}}}}));
    runtime.evaluate(time(5));
    CHECK(snapshots.size() == 3);
    runtime.stop();
}

TEST_CASE("native TSC queues append nullable cells for every frame cell type",
          "[feature:frame][series:tsc][queue][scalar:bool][scalar:i64][scalar:double][scalar:string][scalar:symbol]") {
    struct Snapshot {
        bool integer_valid{};
        EvaluationTime integer_last_updated{MIN_DT};
        std::vector<std::optional<bool>> booleans;
        std::vector<std::optional<i64>> integers;
        std::vector<std::optional<double>> doubles;
        std::vector<std::optional<std::string>> strings;
        std::vector<std::optional<symbol>> symbols;
    };

    const auto long_text = std::string(320, 's');
    auto accepted = std::array<bool, 7>{};
    const auto booleans = queue<TSC<bool>>(start = [&](Queue output) {
        accepted[0] = output.write(literal::TimeSeriesLiteral{true, false, NULL_VALUE});
    });
    const auto integers = queue<TSC<i64>>(start = [&](Queue output) {
        accepted[1] = output.write(literal::TimeSeriesLiteral{i64{10}, NULL_VALUE, i64{30}});
        accepted[2] = output.write(literal::TimeSeriesLiteral(literal::INVALID));
        accepted[3] = output.write(literal::TimeSeriesLiteral{});
    });
    const auto doubles = queue<TSC<double>>(start = [&](Queue output) {
        accepted[4] = output.write(literal::TimeSeriesLiteral{1.25, NULL_VALUE, 3.5});
    });
    const auto strings = queue<TSC<string>>(start = [&](Queue output) {
        accepted[5] = output.write(
            literal::TimeSeriesLiteral{"short", NULL_VALUE, std::string_view{long_text}});
    });
    const auto symbols = queue<TSC<symbol>>(start = [&](Queue output) {
        accepted[6] = output.write(literal::TimeSeriesLiteral{symbol{"first"}, NULL_VALUE, symbol{"third"}});
    });
    auto snapshots = std::vector<Snapshot>{};
    const auto observe = node([&](In<TSC<bool>, optional> bool_input, In<TSC<i64>, optional> integer_input,
                                  In<TSC<double>, optional> double_input, In<TSC<string>, optional> string_input,
                                  In<TSC<symbol>, optional> symbol_input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        auto snapshot = Snapshot{.integer_valid = integer_input.valid(),
                                 .integer_last_updated = integer_input.last_updated()};
        const auto collect = [](const auto &input, auto &cells, auto convert) {
            if (!input.valid())
                return;
            input.for_each_row([&](row_id_t id, std::size_t) { cells.push_back(convert(input.cell(id))); });
        };
        collect(bool_input, snapshot.booleans, [](auto cell) { return cell; });
        collect(integer_input, snapshot.integers, [](auto cell) { return cell; });
        collect(double_input, snapshot.doubles, [](auto cell) { return cell; });
        collect(string_input, snapshot.strings, [](auto cell) {
            return cell ? std::optional<std::string>{*cell} : std::nullopt;
        });
        collect(symbol_input, snapshot.symbols, [](auto cell) { return cell; });
        snapshots.push_back(std::move(snapshot));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        return observe(booleans(), integers(), doubles(), strings(), symbols(), cycle);
    };

    const auto result = run_graph(graph, input<TS<i64>>({0, 1, 2, 3}));
    static_cast<void>(result);
    CHECK(std::ranges::all_of(accepted, std::identity{}));
    REQUIRE(snapshots.size() >= 4);
    CHECK(snapshots[0].integer_valid);
    CHECK(snapshots[0].booleans == std::vector<std::optional<bool>>{true, false, std::nullopt});
    CHECK(snapshots[0].integers == std::vector<std::optional<i64>>{10, std::nullopt, 30});
    CHECK(std::ranges::any_of(snapshots, [](const auto &snapshot) {
        return snapshot.doubles == std::vector<std::optional<double>>{1.25, std::nullopt, 3.5};
    }));
    CHECK(std::ranges::any_of(snapshots, [&](const auto &snapshot) {
        return snapshot.strings == std::vector<std::optional<std::string>>{"short", std::nullopt, long_text};
    }));
    CHECK(std::ranges::any_of(snapshots, [](const auto &snapshot) {
        return snapshot.symbols == std::vector<std::optional<symbol>>{symbol{"first"}, std::nullopt, symbol{"third"}};
    }));
    const auto first_integer = std::ranges::find_if(snapshots, [](const auto &snapshot) {
        return snapshot.integer_valid &&
               snapshot.integers == std::vector<std::optional<i64>>{10, std::nullopt, 30};
    });
    const auto invalid_integer = std::ranges::find_if(snapshots, [](const auto &snapshot) {
        return !snapshot.integer_valid;
    });
    const auto empty_integer = std::ranges::find_if(snapshots, [](const auto &snapshot) {
        return snapshot.integer_valid && snapshot.integers.empty();
    });
    REQUIRE(first_integer != snapshots.end());
    REQUIRE(invalid_integer != snapshots.end());
    REQUIRE(empty_integer != snapshots.end());
    CHECK(first_integer < invalid_integer);
    CHECK(invalid_integer < empty_integer);
    CHECK(first_integer->integer_last_updated < invalid_integer->integer_last_updated);
    CHECK(invalid_integer->integer_last_updated < empty_integer->integer_last_updated);
}

TEST_CASE("wiring-time TSF schemas and nested collections use native queue operations",
          "[feature:frame][series:tsf][series:tsd][queue][schema:unnamed][transition:addition]") {
    const auto fields = std::array{SchemaField{util::symbol{"text"}, &cpp_scalar_type<string>()},
                                   SchemaField{util::symbol{"value"}, &cpp_scalar_type<double>()}};
    const auto &schema = anonymous_schema(fields);
    const auto &type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    auto accepted = false;
    const auto source = queue(output_type = type,
                              start = [&](Queue output) {
                                  accepted = output.write(literal::TimeSeriesLiteral{
                                      {{"text", "first"}, {"value", 1.0}},
                                      {{"text", "second"}, {"value", 2.0}}});
                              },
                              mode = QueueMode::standard);
    auto observations = std::vector<std::vector<std::pair<std::string, double>>>{};
    const auto observe = node([&](In<TSF<SCHEMA>, optional> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        auto rows = std::vector<std::pair<std::string, double>>{};
        if (input.valid()) {
            const auto text = input.column("text").input<string>();
            const auto value = input.column("value").input<double>();
            input.for_each_row([&](row_id_t id, std::size_t) { rows.emplace_back(*text.cell(id), *value.cell(id)); });
        }
        observations.push_back(std::move(rows));
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(*source(), cycle); };

    const auto result = run_graph(graph, input<TS<i64>>({0}));
    CHECK(result.equals<i64>({0}));
    CHECK(accepted);
    REQUIRE(observations.size() == 1);
    CHECK(observations.back() == std::vector<std::pair<std::string, double>>{{"first", 1.0}, {"second", 2.0}});
}

TEST_CASE("eliding TSC queues append cells in write order",
          "[feature:frame][series:tsc][queue][queue:elide][transition:update]") {
    auto values = std::vector<std::optional<i64>>{};
    auto accepted = std::array<bool, 3>{};
    const auto source = queue<TSC<i64>>(
        start = [&](Queue output) {
            accepted[0] = output.write(literal::TimeSeriesLiteral{i64{10}});
            accepted[1] = output.write(literal::TimeSeriesLiteral{i64{20}});
            accepted[2] = output.write(literal::TimeSeriesLiteral{i64{30}});
        },
        mode = QueueMode::elide);
    const auto observe = node([&](In<TSC<i64>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        input.for_each_row([&](row_id_t id, std::size_t) { values.push_back(input.cell(id)); });
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(source(), cycle); };
    const auto result = run_graph(graph, input<TS<i64>>({0}));
    CHECK(result.equals<i64>({0}));
    CHECK(std::ranges::all_of(accepted, std::identity{}));
    CHECK(values == std::vector<std::optional<i64>>{10, 20, 30});
}

TEST_CASE("nested TSF queue changes apply through a native TSD",
          "[feature:frame][series:tsf][series:tsd][queue][transition:addition][transition:invalidation]") {
    auto accepted = std::array<bool, 3>{};
    const auto source = queue<TSD<i64, TSF<QueueRows>>>(
        start = [&](Queue output) {
            accepted[0] = output.write(
                literal::TimeSeriesLiteral{{7, {{{"id", i64{3}}, {"text", "nested"}, {"value", 8.5}}}}});
            accepted[1] = output.write(literal::TimeSeriesLiteral(literal::INVALID));
            accepted[2] = output.write(
                literal::TimeSeriesLiteral{{7, {{{"id", i64{4}}, {"text", "reinserted"}, {"value", 9.5}}}}});
        },
        mode = QueueMode::standard);
    auto snapshots = std::vector<std::optional<std::vector<QueueRow>>>{};
    auto last_updates = std::vector<EvaluationTime>{};
    const auto observe = node([&](In<TSD<i64, TSF<QueueRows>>, optional> input, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        if (!input.valid()) {
            last_updates.push_back(input.last_updated());
            snapshots.emplace_back(std::nullopt);
            output.set(*cycle);
            return;
        }
        if (!input.contains(7)) {
            last_updates.push_back(input.last_updated());
            snapshots.emplace_back(std::vector<QueueRow>{});
            output.set(*cycle);
            return;
        }
        const auto frame = input[7];
        auto rows = std::vector<QueueRow>{};
        const auto ids = frame.column("id").input<i64>();
        const auto text = frame.column("text").input<string>();
        const auto values = frame.column("value").input<double>();
        frame.for_each_row([&](row_id_t id, std::size_t) {
            const auto text_cell = text.cell(id);
            rows.push_back({*ids.cell(id), text_cell ? std::optional<std::string>{*text_cell} : std::nullopt,
                            values.cell(id)});
        });
        snapshots.emplace_back(std::move(rows));
        last_updates.push_back(input.last_updated());
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(source(), cycle); };
    const auto result = run_graph(graph, input<TS<i64>>({0, 1, 2}));
    static_cast<void>(result);
    CHECK(std::ranges::all_of(accepted, std::identity{}));
    REQUIRE(snapshots.size() >= 3);
    const auto initial = std::ranges::find(snapshots, std::optional<std::vector<QueueRow>>{
                                                        std::vector<QueueRow>{{3, "nested", 8.5}}});
    const auto invalid = std::ranges::find_if(snapshots, [](const auto &snapshot) { return !snapshot; });
    const auto reinserted = std::ranges::find(snapshots, std::optional<std::vector<QueueRow>>{
                                                           std::vector<QueueRow>{{4, "reinserted", 9.5}}});
    REQUIRE(initial != snapshots.end());
    REQUIRE(invalid != snapshots.end());
    REQUIRE(reinserted != snapshots.end());
    CHECK(initial < invalid);
    CHECK(invalid < reinserted);
    const auto initial_index = static_cast<std::size_t>(std::distance(snapshots.begin(), initial));
    const auto invalid_index = static_cast<std::size_t>(std::distance(snapshots.begin(), invalid));
    const auto reinserted_index = static_cast<std::size_t>(std::distance(snapshots.begin(), reinserted));
    CHECK(last_updates[initial_index] < last_updates[invalid_index]);
    CHECK(last_updates[invalid_index] < last_updates[reinserted_index]);
}

TEST_CASE("nested TSC queues apply through TSB and fixed TSL runtimes",
          "[feature:frame][series:tsc][series:tsb][series:fixed-tsl][queue][transition:addition]") {
    auto accepted = std::array<bool, 2>{};
    const auto bundle_source = queue<TSB<QueueColumns>>(
        start = [&](Queue output) {
            accepted[0] = output.write(literal::TimeSeriesLiteral{
                {"integers", {i64{10}, i64{20}}}, {"strings", {"first", "second"}}});
        },
        mode = QueueMode::standard);
    const auto list_source = queue<TSL<TSC<i64>, 2>>(
        start = [&](Queue output) {
            accepted[1] = output.write(literal::TimeSeriesLiteral{{0, {i64{30}, i64{40}}}, {1, {i64{50}}}});
        },
        mode = QueueMode::standard);
    auto bundle_integers = std::vector<std::optional<i64>>{};
    auto bundle_strings = std::vector<std::optional<std::string>>{};
    auto list_values = std::array<std::vector<std::optional<i64>>, 2>{};
    const auto observe = node([&](In<TSB<QueueColumns>> bundle, In<TSL<TSC<i64>, 2>> list, In<TS<i64>> cycle,
                                  Out<TS<i64>> output) {
        bundle->integers.for_each_row([&](row_id_t id, std::size_t) {
            bundle_integers.push_back(bundle->integers.cell(id));
        });
        bundle->strings.for_each_row([&](row_id_t id, std::size_t) {
            const auto cell = bundle->strings.cell(id);
            bundle_strings.push_back(cell ? std::optional<std::string>{*cell} : std::nullopt);
        });
        for (auto index = std::size_t{}; index != list.size(); ++index) {
            const auto cells = list[index];
            cells.for_each_row([&](row_id_t id, std::size_t) { list_values[index].push_back(cells.cell(id)); });
        }
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(bundle_source(), list_source(), cycle); };
    const auto result = run_graph(graph, input<TS<i64>>({0}));

    CHECK(result.equals<i64>({0}));
    CHECK(std::ranges::all_of(accepted, std::identity{}));
    CHECK(bundle_integers == std::vector<std::optional<i64>>{10, 20});
    CHECK(bundle_strings == std::vector<std::optional<std::string>>{"first", "second"});
    CHECK(list_values[0] == std::vector<std::optional<i64>>{30, 40});
    CHECK(list_values[1] == std::vector<std::optional<i64>>{50});
}

TEST_CASE("zero-column TSF queues preserve empty rows",
          "[feature:frame][series:tsf][queue][schema:zero-column][transition:addition]") {
    auto accepted = false;
    auto size = std::size_t{};
    const auto source = queue<TSF<EmptyQueueRows>>(
        start = [&](Queue output) {
            accepted = output.write(literal::TimeSeriesLiteral{literal::ScalarLiteral{}, literal::ScalarLiteral{}});
        },
        mode = QueueMode::standard);
    const auto observe = node([&](In<TSF<EmptyQueueRows>> input, In<TS<i64>> cycle, Out<TS<i64>> output) {
        size = input.size();
        output.set(*cycle);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return observe(source(), cycle); };
    const auto result = run_graph(graph, input<TS<i64>>({0}));

    CHECK(result.equals<i64>({0}));
    CHECK(accepted);
    CHECK(size == 2);
}

TEST_CASE("frame queue runtimes accept native storage and reject borrowed or computed TSC storage",
          "[feature:frame][series:tsc][queue][storage:native][storage:borrowed][storage:computed][failure]") {
    const auto &type = TSC<i64>::type();
    const auto &native_runtime = column_time_series_runtime_type<i64>(type);
    const auto &borrowed_runtime = column_view_time_series_runtime_type(type);
    const auto &computed_runtime = column_computed_time_series_runtime_type<i64>(type);

    const auto accepts_runtime = [&](const TimeSeriesRuntimeType &runtime_type) {
        const auto source = node(RuntimeQueueSource{runtime_type});
        auto wiring = GraphWiring{};
        std::optional<WiringPoint> output;
        {
            const auto context = WiringContext{wiring};
            output = source();
        }
        wiring.finalize();
        const auto target = wiring.output_node_index(*output);
        const auto compiled = CompiledGraph{wiring};
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        auto accepted = true;
        try {
            static_cast<void>(runtime.make_queue(target));
        } catch (const std::invalid_argument &) {
            accepted = false;
        }
        runtime.stop();
        return accepted;
    };

    CHECK(accepts_runtime(native_runtime));
    CHECK_FALSE(accepts_runtime(borrowed_runtime));
    CHECK_FALSE(accepts_runtime(computed_runtime));
}

TEST_CASE("TSF queue payloads own string cells through evaluation and unread discard",
          "[feature:frame][series:tsf][queue][scalar:string][ownership]") {
    auto snapshots = std::vector<std::vector<QueueRow>>{};
    const auto source = queue<TSF<PlainQueueRows>>(start = [](Queue) {}, mode = QueueMode::standard);
    const auto observe = node([&](In<TSF<PlainQueueRows>> input) {
        auto rows = std::vector<QueueRow>{};
        const auto text = input.column("text").input<string>();
        const auto values = input.column("value").input<double>();
        input.for_each_row([&](row_id_t id, std::size_t) {
            const auto cell = text.cell(id);
            rows.push_back({static_cast<i64>(id), cell ? std::optional<std::string>{*cell} : std::nullopt,
                            values.cell(id)});
        });
        snapshots.push_back(std::move(rows));
    });
    auto wiring = GraphWiring{};
    WiringPoint output;
    {
        const auto context = WiringContext{wiring};
        output = source();
        observe(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    auto expected = std::string{};
    {
        auto producer = std::string(320, 'p');
        expected = producer;
        REQUIRE(queue.write(
            literal::TimeSeriesLiteral{{{"text", std::string_view{producer}}, {"value", 1.0}}}));
    }
    runtime.evaluate(time(2));
    CHECK(snapshots == std::vector<std::vector<QueueRow>>{{{0, expected, 1.0}}});

    auto rejected_text = std::string(320, 'r');
    CHECK_THROWS_AS(queue.write(literal::TimeSeriesLiteral{
                          {{"text", std::string_view{rejected_text}}, {"value", "not a number"}}}),
                    std::invalid_argument);
    rejected_text.assign(320, 'x');

    auto next_text = std::string(320, 'n');
    const auto next_expected = next_text;
    REQUIRE(queue.write(
        literal::TimeSeriesLiteral{{{"text", std::string_view{next_text}}, {"value", 2.0}}}));
    next_text.assign(320, 'x');
    runtime.evaluate(time(3));
    CHECK(snapshots == std::vector<std::vector<QueueRow>>{{{0, expected, 1.0}}, { {0, expected, 1.0},
                                                                                {1, next_expected, 2.0}}});

    auto memory_resource = CountingResource{};
    {
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{compiled, allocator{memory_resource}, global_state};
        runtime.start(time(4));
        {
            auto queue = runtime.make_queue(target);
            auto unread_text = std::string(320, 'u');
            REQUIRE(queue.write(
                literal::TimeSeriesLiteral{{{"text", std::string_view{unread_text}}, {"value", 3.0}}}));
            unread_text.assign(320, 'x');
            runtime.stop();
            CHECK_FALSE(queue.write(literal::TimeSeriesLiteral{}));
        }
    }
    CHECK(memory_resource.allocations == memory_resource.deallocations);
}
