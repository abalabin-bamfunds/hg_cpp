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

namespace {

using namespace bg::literals;

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("queue nodes bind stop scalars to named start scalars and close stop handles",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][node]") {
    struct QueueCallbackProbe {
        bg::i64 started_id{};
        bg::i64 stopped_id{};
        std::string started_topic;
        std::string stopped_topic;
        bool start_write_accepted{};
        bool stop_write_accepted{true};
        bg::Queue retained;
    };

    auto probe = QueueCallbackProbe{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(
        bg::start.bind<"id"_n, "topic"_n>([&probe](bg::i64 id, std::string_view topic, bg::Queue queue) {
            probe.started_id = id;
            probe.started_topic = topic;
            probe.start_write_accepted = queue.write(::literal::TimeSeriesLiteral(bg::i64{id}));
            probe.retained = std::move(queue);
        }),
        bg::stop.bind<"topic"_n, "id"_n>([&probe](std::string_view topic, bg::Queue queue, bg::i64 id) noexcept {
            probe.stopped_id = id;
            probe.stopped_topic = topic;
            probe.stop_write_accepted = queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE));
        }),
        bg::mode = bg::QueueMode::elide);

    static_assert(std::same_as<decltype(queue_node(bg::i64{}, std::string{})), bg::wp<bg::TS<bg::i64>>>);

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    bg::WiringPoint duplicate_output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node(bg::i64{37}, std::string{"prices"});
        duplicate_output = queue_node(bg::i64{37}, std::string{"prices"});
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    CHECK(wiring.output_node_index(duplicate_output) == target);
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node(target).queue_mode() == bg::QueueMode::elide);
    CHECK(compiled.node(target).has_output());
    CHECK(compiled.node(target).evaluation_function() == nullptr);
    CHECK(compiled.node(target).parameters().size() == 2);

    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    CHECK(probe.started_id == 37);
    CHECK(probe.started_topic == "prices");
    CHECK(probe.start_write_accepted);
    CHECK_FALSE(runtime.next_scheduled_time().has_value());

    runtime.stop();

    CHECK(probe.stopped_id == 37);
    CHECK(probe.stopped_topic == "prices");
    CHECK_FALSE(probe.stop_write_accepted);
    CHECK_FALSE(probe.retained.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
}

TEST_CASE("queue node declarations route writes to their declared output types and modes",
          "[evaluation][feature:time-series-storage][series:value][scalar:boolean][scalar:i64][queue][node]") {
    struct Probe {
        bg::Queue integer_queue;
        bg::Queue boolean_queue;
        bool integer_write_accepted{};
        bool boolean_write_accepted{};
    } probe;

    const auto integer_node = bg::queue<bg::TS<bg::i64>>(
        bg::start = [&probe](bg::Queue queue) {
            probe.integer_queue = queue;
            probe.integer_write_accepted = queue.write(::literal::TimeSeriesLiteral(bg::i64{19}));
        },
        bg::mode = bg::QueueMode::standard);
    const auto boolean_node = bg::queue<bg::TS<bool>>(
        bg::start = [&probe](bg::Queue queue) {
            probe.boolean_queue = queue;
            probe.boolean_write_accepted = queue.write(::literal::TimeSeriesLiteral(true));
        },
        bg::mode = bg::QueueMode::elide);

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint integer_output;
    bg::WiringPoint boolean_output;
    {
        const auto context = bg::WiringContext{wiring};
        integer_output = integer_node();
        boolean_output = boolean_node();
    }
    wiring.finalize();
    const auto integer_target = wiring.output_node_index(integer_output);
    const auto boolean_target = wiring.output_node_index(boolean_output);
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node(integer_target).queue_mode() == bg::QueueMode::standard);
    CHECK(compiled.node(boolean_target).queue_mode() == bg::QueueMode::elide);

    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(5));

    CHECK(probe.integer_write_accepted);
    CHECK(probe.boolean_write_accepted);
    CHECK(probe.integer_queue.write(::literal::TimeSeriesLiteral(bg::i64{20})));
    CHECK(probe.boolean_queue.write(::literal::TimeSeriesLiteral(false)));

    runtime.stop();
}

TEST_CASE("queue factories reject unsupported modes and time-series scalar inputs",
          "[evaluation][feature:node-api][queue][wiring]") {
    const auto start_callback = [](bg::Queue) {};
    CHECK_THROWS_AS(bg::queue<bg::TS<bg::i64>>(
                        bg::start = start_callback, bg::mode = static_cast<bg::QueueMode>(41)),
                    std::invalid_argument);

    const auto invalid_batch = bg::queue<bg::TS<bg::i64>>(
        bg::start = start_callback, bg::mode = bg::QueueMode::batch);
    auto invalid_batch_wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{invalid_batch_wiring};
        CHECK_THROWS_AS((void)invalid_batch(), bg::WiringError);
    }

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto scalar_queue = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::i64, bg::Queue) {});
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        CHECK_THROWS_AS(scalar_queue(source()), bg::TypeResolutionError);
    }
}

TEST_CASE("queue declarations require ordinary guarantees in every mode",
          "[feature:node-api][queue][wiring][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto require_rejected = [](auto definition) {
        auto wiring = bg::GraphWiring{};
        const auto context = bg::WiringContext{wiring};
        CHECK_THROWS_AS((void)definition(), bg::WiringError);
    };

    for (const auto mode : {bg::QueueMode::standard, bg::QueueMode::elide, bg::QueueMode::batch}) {
        for (const auto guarantee : {bg::TimeSeriesGuarantee::deduplicated, bg::TimeSeriesGuarantee::final,
                                     bg::TimeSeriesGuarantee::const_}) {
            const auto &output_type = bg::TS<bg::List<bg::i64>>::type().with_guarantee(guarantee);
            require_rejected(bg::queue(bg::output_type = output_type, bg::start = [](bg::Queue) {},
                                       bg::mode = mode));
        }
        require_rejected(bg::queue<bg::TSD<bg::i64, bg::TS<bg::List<bg::i64>, bg::TimeSeriesGuarantee::final>>>(
            bg::start = [](bg::Queue) {}, bg::mode = mode));
    }
}

TEST_CASE("runtime queue declarations return an untyped output wiring handle",
          "[feature:node-api][series:value][queue][node]") {
    const auto queue_node = bg::queue(
        bg::output_type = bg::TS<bg::i64>::type(),
        bg::start = [](bg::Queue) {});
    const auto batch_queue_node = bg::queue(
        bg::output_type = bg::TS<bg::List<bg::i64>>::type(),
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    static_assert(std::same_as<decltype(queue_node()), std::optional<bg::WiringPoint>>);

    auto wiring = bg::GraphWiring{};
    std::optional<bg::WiringPoint> output;
    std::optional<bg::WiringPoint> batch_output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        batch_output = batch_queue_node();
    }
    wiring.finalize();
    REQUIRE(output.has_value());
    CHECK(wiring.output_node_index(*output) == 0);
    REQUIRE(batch_output.has_value());
    CHECK(wiring.output_node_index(*batch_output) == 1);
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node(0).has_output());
    CHECK(compiled.node(0).has_output_runtime_type());
    CHECK(compiled.node(0).evaluation_function() == nullptr);
    CHECK(&compiled.node(0).output_runtime_type() == &bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>());
    CHECK(compiled.node(1).queue_mode() == bg::QueueMode::batch);
    CHECK(&compiled.node(1).output_runtime_type() ==
          &bg::cpp_time_series_runtime_type<bg::TS<bg::List<bg::i64>>>());
}

TEST_CASE("queue stop callbacks bind optional scalars and Scalar views by canonical type",
          "[evaluation][feature:node-api][feature:scalar-api][queue][scalar:boolean][scalar:i64][scalar:string]"
          "[validity:optional]") {
    struct Probe {
        std::optional<bool> started_enabled;
        std::optional<bool> stopped_enabled;
        bg::i64 started_id{};
        bg::i64 stopped_id{};
        std::string started_topic;
        std::string stopped_topic;
        bool start_write_accepted{};
        bool stop_write_accepted{true};
    };

    for (const auto enabled : {std::optional<bool>{true}, std::optional<bool>{}}) {
        auto probe = Probe{};
        const auto queue_node = bg::queue<bg::TS<bg::i64>>(
            bg::start = [&probe](std::optional<bool> active, bg::Queue queue, bg::Scalar<bg::i64> id,
                                 std::string_view topic) {
                probe.started_enabled = active;
                probe.started_id = id.value();
                probe.started_topic = topic;
                if (active.value_or(false)) {
                    probe.start_write_accepted = queue.write(::literal::TimeSeriesLiteral(id.value()));
                }
            },
            bg::stop = [&probe](std::string_view topic, std::optional<bool> active, bg::Queue queue,
                                bg::i64 id) noexcept {
                probe.stopped_enabled = active;
                probe.stopped_id = id;
                probe.stopped_topic = topic;
                probe.stop_write_accepted = queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE));
            });

        auto wiring = bg::GraphWiring{};
        {
            const auto context = bg::WiringContext{wiring};
            (void)queue_node(enabled, bg::i64{52}, std::string{"orders"});
        }
        wiring.finalize();
        const auto compiled = bg::CompiledGraph{wiring};
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
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
    auto selected = bg::i64{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::i64, bool, bg::i64, bg::Queue) {},
        bg::stop.bind<2>([&selected](bg::i64 value) noexcept { selected = value; }));

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)queue_node(bg::i64{11}, true, bg::i64{29});
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(3));
    runtime.stop();

    CHECK(selected == 29);
}

TEST_CASE("queue handles retain independent runtime lifetimes after stop",
          "[evaluation][feature:node-api][queue][lifecycle]") {
    auto handles = std::vector<bg::Queue>{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(
        bg::start = [&handles](bg::Queue queue) { handles.push_back(queue); });
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)queue_node();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto first_runtime = bg::RuntimeGraph{compiled, global_state};
    auto second_runtime = bg::RuntimeGraph{compiled, global_state};
    first_runtime.start(time(4));
    second_runtime.start(time(4));
    REQUIRE(handles.size() == 2);
    auto first_copy = handles[0];
    auto second_copy = handles[1];

    first_runtime.stop();

    CHECK_FALSE(handles[0].write(::literal::TimeSeriesLiteral(bg::i64{1})));
    CHECK_FALSE(first_copy.write(::literal::TimeSeriesLiteral(bg::i64{2})));
    CHECK(handles[1].write(::literal::TimeSeriesLiteral(bg::i64{3})));
    CHECK(second_copy.write(::literal::TimeSeriesLiteral(bg::i64{4})));

    second_runtime.stop();

    CHECK_FALSE(handles[1].write(::literal::TimeSeriesLiteral(bg::i64{5})));
    CHECK_FALSE(second_copy.write(::literal::TimeSeriesLiteral(bg::i64{6})));
}
