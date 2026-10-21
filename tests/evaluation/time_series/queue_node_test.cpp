#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/queue.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/named_argument.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace bg;

namespace {

using namespace literals;

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("queue node declarations route writes to their declared output types and modes",
          "[evaluation][feature:time-series-storage][series:value][scalar:boolean][scalar:i64][queue][node]") {
    struct Probe {
        Queue integer_queue;
        Queue boolean_queue;
        bool integer_write_accepted{};
        bool boolean_write_accepted{};
    } probe;

    const auto integer_node = queue<TS<i64>>(
        start =
            [&probe](Queue queue) {
                probe.integer_queue = queue;
                probe.integer_write_accepted = queue.write(::literal::TimeSeriesLiteral(i64{19}));
            },
        mode = QueueMode::standard);
    const auto boolean_node = queue<TS<bool>>(
        start =
            [&probe](Queue queue) {
                probe.boolean_queue = queue;
                probe.boolean_write_accepted = queue.write(::literal::TimeSeriesLiteral(true));
            },
        mode = QueueMode::elide);

    auto wiring = GraphWiring{};
    WiringPoint integer_output;
    WiringPoint boolean_output;
    {
        const auto context = WiringContext{wiring};
        integer_output = integer_node();
        boolean_output = boolean_node();
    }
    wiring.finalize();
    const auto integer_target = wiring.output_node_index(integer_output);
    const auto boolean_target = wiring.output_node_index(boolean_output);
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node(integer_target).queue_mode() == QueueMode::standard);
    CHECK(compiled.node(boolean_target).queue_mode() == QueueMode::elide);

    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.start(time(5));

    CHECK(probe.integer_write_accepted);
    CHECK(probe.boolean_write_accepted);
    CHECK(probe.integer_queue.write(::literal::TimeSeriesLiteral(i64{20})));
    CHECK(probe.boolean_queue.write(::literal::TimeSeriesLiteral(false)));

    runtime.stop();
}

TEST_CASE("queue nodes bind stop scalars to named start scalars and close stop handles",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][node]") {
    struct QueueCallbackProbe {
        i64 started_id{};
        i64 stopped_id{};
        std::string started_topic;
        std::string stopped_topic;
        bool start_write_accepted{};
        bool stop_write_accepted{true};
        Queue retained;
    };

    auto probe = QueueCallbackProbe{};
    const auto queue_node =
        queue<TS<i64>>(start.bind<"id"_n, "topic"_n>([&probe](i64 id, std::string_view topic, Queue queue) {
            probe.started_id = id;
            probe.started_topic = topic;
            probe.start_write_accepted = queue.write(::literal::TimeSeriesLiteral(i64{id}));
            probe.retained = std::move(queue);
        }),
                       stop.bind<"topic"_n, "id"_n>([&probe](std::string_view topic, Queue queue, i64 id) noexcept {
                           probe.stopped_id = id;
                           probe.stopped_topic = topic;
                           probe.stop_write_accepted = queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE));
                       }),
                       mode = QueueMode::elide);

    static_assert(std::same_as<decltype(queue_node(i64{}, std::string{})), wp<TS<i64>>>);

    auto wiring = GraphWiring{};
    WiringPoint output;
    WiringPoint duplicate_output;
    {
        const auto context = WiringContext{wiring};
        output = queue_node(i64{37}, std::string{"prices"});
        duplicate_output = queue_node(i64{37}, std::string{"prices"});
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    CHECK(wiring.output_node_index(duplicate_output) == target);
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node(target).queue_mode() == QueueMode::elide);
    CHECK(compiled.node(target).has_output());
    CHECK(compiled.node(target).evaluation_function() == nullptr);
    CHECK(compiled.node(target).parameters().size() == 2);

    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    CHECK(probe.started_id == 37);
    CHECK(probe.started_topic == "prices");
    CHECK(probe.start_write_accepted);
    CHECK(runtime.next_scheduled_time() == time(2));

    runtime.stop();

    CHECK(probe.stopped_id == 37);
    CHECK(probe.stopped_topic == "prices");
    CHECK_FALSE(probe.stop_write_accepted);
    CHECK_FALSE(probe.retained.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
}

TEST_CASE("queue factories reject unsupported modes and time-series scalar inputs",
          "[evaluation][feature:node-api][queue][wiring]") {
    const auto start_callback = [](Queue) {};
    CHECK_THROWS_AS(queue<TS<i64>>(start = start_callback, mode = static_cast<QueueMode>(41)), std::invalid_argument);

    const auto invalid_batch = queue<TS<i64>>(start = start_callback, mode = QueueMode::batch);
    auto invalid_batch_wiring = GraphWiring{};
    {
        const auto context = WiringContext{invalid_batch_wiring};
        CHECK_THROWS_AS((void)invalid_batch(), WiringError);
    }

    const auto source = node([](Out<TS<i64>>) {});
    const auto scalar_queue = queue<TS<i64>>(start = [](i64, Queue) {});
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        CHECK_THROWS_AS(scalar_queue(source()), TypeResolutionError);
    }
}

TEST_CASE("queue declarations require ordinary guarantees in every mode",
          "[feature:node-api][queue][wiring][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto require_rejected = [](auto definition) {
        auto wiring = GraphWiring{};
        const auto context = WiringContext{wiring};
        CHECK_THROWS_AS((void)definition(), WiringError);
    };

    for (const auto mode : {QueueMode::standard, QueueMode::elide, QueueMode::batch}) {
        for (const auto guarantee :
             {TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_}) {
            const auto &output_type = TS<List<i64>>::type().with_guarantee(guarantee);
            require_rejected(queue(output_type = output_type, start = [](Queue) {}, mode = mode));
        }
        require_rejected(queue<TSD<i64, TS<List<i64>, TimeSeriesGuarantee::final>>>(start = [](Queue) {}, mode = mode));
    }
}

TEST_CASE("runtime queue declarations return an untyped output wiring handle",
          "[feature:node-api][series:value][queue][node]") {
    const auto queue_node = queue(output_type = TS<i64>::type(), start = [](Queue) {});
    const auto batch_queue_node =
        queue(output_type = TS<List<i64>>::type(), start = [](Queue) {}, mode = QueueMode::batch);
    static_assert(std::same_as<decltype(queue_node()), std::optional<WiringPoint>>);

    auto wiring = GraphWiring{};
    std::optional<WiringPoint> output;
    std::optional<WiringPoint> batch_output;
    {
        const auto context = WiringContext{wiring};
        output = queue_node();
        batch_output = batch_queue_node();
    }
    wiring.finalize();
    REQUIRE(output.has_value());
    CHECK(wiring.output_node_index(*output) == 0);
    REQUIRE(batch_output.has_value());
    CHECK(wiring.output_node_index(*batch_output) == 1);
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node(0).has_output());
    CHECK(compiled.node(0).has_output_runtime_type());
    CHECK(compiled.node(0).evaluation_function() == nullptr);
    CHECK(&compiled.node(0).output_runtime_type() == &cpp_time_series_runtime_type<TS<i64>>());
    CHECK(compiled.node(1).queue_mode() == QueueMode::batch);
    CHECK(&compiled.node(1).output_runtime_type() == &cpp_time_series_runtime_type<TS<List<i64>>>());
}

TEST_CASE("queue stop callbacks bind optional scalars and Scalar views by canonical type",
          "[evaluation][feature:node-api][feature:scalar-api][queue][scalar:boolean][scalar:i64][scalar:string]"
          "[validity:optional]") {
    struct Probe {
        std::optional<bool> started_enabled;
        std::optional<bool> stopped_enabled;
        i64 started_id{};
        i64 stopped_id{};
        std::string started_topic;
        std::string stopped_topic;
        bool start_write_accepted{};
        bool stop_write_accepted{true};
    };

    for (const auto enabled : {std::optional<bool>{true}, std::optional<bool>{}}) {
        auto probe = Probe{};
        const auto queue_node = queue<TS<i64>>(
            start =
                [&probe](std::optional<bool> active, Queue queue, Scalar<i64> id, std::string_view topic) {
                    probe.started_enabled = active;
                    probe.started_id = id.value();
                    probe.started_topic = topic;
                    if (active.value_or(false)) {
                        probe.start_write_accepted = queue.write(::literal::TimeSeriesLiteral(id.value()));
                    }
                },
            stop =
                [&probe](std::string_view topic, std::optional<bool> active, Queue queue, i64 id) noexcept {
                    probe.stopped_enabled = active;
                    probe.stopped_id = id;
                    probe.stopped_topic = topic;
                    probe.stop_write_accepted = queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE));
                });

        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            (void)queue_node(enabled, i64{52}, std::string{"orders"});
        }
        wiring.finalize();
        const auto compiled = CompiledGraph{wiring};
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{compiled, global_state};
        runtime.start(time(2));
        runtime.stop();

        CHECK(probe.started_enabled == enabled);
        CHECK(probe.stopped_enabled == enabled);
        CHECK(probe.started_id == 52);
        CHECK(probe.stopped_id == 52);
        CHECK(probe.started_topic == "orders");
        CHECK(probe.stopped_topic == "orders");
        CHECK(probe.start_write_accepted == enabled.value_or(false));
        CHECK_FALSE(probe.stop_write_accepted);
    }
}

TEST_CASE("queue stop.bind selects among duplicate canonical start scalar types",
          "[evaluation][feature:node-api][queue][scalar:i64][scalar:boolean][binding:direct]") {
    auto selected = i64{};
    const auto queue_node = queue<TS<i64>>(
        start = [](i64, bool, i64, Queue) {}, stop.bind<2>([&selected](i64 value) noexcept { selected = value; }));

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)queue_node(i64{11}, true, i64{29});
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    runtime.start(time(3));
    runtime.stop();

    CHECK(selected == 29);
}

TEST_CASE("queue handles retain independent runtime lifetimes after stop",
          "[evaluation][feature:node-api][queue][lifecycle]") {
    auto handles = std::vector<Queue>{};
    const auto queue_node = queue<TS<i64>>(start = [&handles](Queue queue) { handles.push_back(queue); });
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        (void)queue_node();
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto first_runtime = RuntimeGraph{compiled, global_state};
    auto second_runtime = RuntimeGraph{compiled, global_state};
    first_runtime.start(time(4));
    second_runtime.start(time(4));
    REQUIRE(handles.size() == 2);
    auto first_copy = handles[0];
    auto second_copy = handles[1];

    first_runtime.stop();

    CHECK_FALSE(handles[0].write(::literal::TimeSeriesLiteral(i64{1})));
    CHECK_FALSE(first_copy.write(::literal::TimeSeriesLiteral(i64{2})));
    CHECK(handles[1].write(::literal::TimeSeriesLiteral(i64{3})));
    CHECK(second_copy.write(::literal::TimeSeriesLiteral(i64{4})));

    second_runtime.stop();

    CHECK_FALSE(handles[1].write(::literal::TimeSeriesLiteral(i64{5})));
    CHECK_FALSE(second_copy.write(::literal::TimeSeriesLiteral(i64{6})));
}
