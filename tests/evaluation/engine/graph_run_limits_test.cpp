#include <bg/evaluation_profiler.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/node.hpp>
#include <bg/queue_node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/graph/wiring_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct ScheduledSource {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }
    void evaluate(bg::EvaluationContext *context) const {
        evaluation_times.push_back(context->evaluation_time());
        if (evaluation_times.size() < 10) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
    void stop(bg::EvaluationContext *context) const noexcept { stop_time = context->evaluation_time(); }
    static void reset() {
        evaluation_times.clear();
        stop_time.reset();
    }

    inline static std::vector<bg::EvaluationTime> evaluation_times;
    inline static std::optional<bg::EvaluationTime> stop_time;
};

void run_scheduled_source(const bg::GraphRunOptions &options) {
    const auto source = bg::node<ScheduledSource>();
    bg::run_graph([&] { (void)source(); }, options);
}

} // namespace

TEST_CASE("graph run stop time excludes cycles at the boundary",
          "[evaluation][simulation][feature:simulation][lifecycle]") {
    ScheduledSource::reset();
    const auto start = time(10);

    run_scheduled_source(bg::GraphRunOptions{.start_time = start, .stop_time = time(13)});

    CHECK(ScheduledSource::evaluation_times == std::vector{start, time(11), time(12)});
    CHECK(ScheduledSource::stop_time == time(12));
}

TEST_CASE("run time is relative to start and the earlier explicit stop time wins",
          "[evaluation][simulation][feature:simulation][lifecycle]") {
    ScheduledSource::reset();
    const auto start = time(20);

    run_scheduled_source(bg::GraphRunOptions{.start_time = start,
                                             .stop_time = time(22),
                                             .run_time = std::chrono::microseconds{5}});

    CHECK(ScheduledSource::evaluation_times == std::vector{start, time(21)});
    CHECK(ScheduledSource::stop_time == time(21));

    ScheduledSource::reset();
    const auto duration_start = time(30);
    run_scheduled_source(bg::GraphRunOptions{.start_time = duration_start,
                                             .stop_time = time(35),
                                             .run_time = std::chrono::microseconds{2}});
    CHECK(ScheduledSource::evaluation_times == std::vector{duration_start, time(31)});
    CHECK(ScheduledSource::stop_time == time(31));
}

TEST_CASE("graph run limits apply through the profiler recorder",
          "[evaluation][simulation][feature:simulation][feature:profiling]") {
    ScheduledSource::reset();
    auto profiler = bg::EvaluationProfiler{};
    const auto start = time(30);

    run_scheduled_source(bg::GraphRunOptions{.recorder = profiler.recorder(),
                                             .start_time = start,
                                             .run_time = std::chrono::microseconds{2}});

    CHECK(ScheduledSource::evaluation_times == std::vector{start, time(31)});
    CHECK(profiler.outer_totals().cycles == 2);
}

TEST_CASE("test graph leaves positions beyond its run limit empty",
          "[evaluation][simulation][feature:simulation][test-graph]") {
    const auto start = time(40);
    const auto actual = bg::testing::test_graph(
        [](bg::wp<bg::TS<bg::i64>> input) { return input; },
        bg::GraphRunOptions{.start_time = start, .stop_time = time(42)},
        std::array{bg::i64{1}, bg::i64{2}, bg::i64{3}, bg::i64{4}});

    CHECK(actual.equals<bg::i64>({1, 2, std::nullopt, std::nullopt}));
}

TEST_CASE("realtime run time ends an idle graph and closes its queue",
          "[evaluation][realtime][feature:realtime][queue][lifecycle]") {
    auto writer = bg::Queue{};
    const auto queue = bg::queue<bg::TS<bg::i64>>(bg::start = [&](bg::Queue value) { writer = std::move(value); });
    const auto before = std::chrono::steady_clock::now();

    bg::run_graph([&] { (void)queue(); },
                  bg::GraphRunOptions{.engine = bg::EvaluationEngine::realtime,
                                      .run_time = std::chrono::milliseconds{40}});

    CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds{20});
    CHECK_FALSE(writer.write(::literal::TimeSeriesLiteral{bg::i64{1}}));
}

TEST_CASE("realtime run limits drain exact timers before the stop time",
          "[evaluation][realtime][feature:realtime][lifecycle]") {
    ScheduledSource::reset();
    const auto start = time(10);

    run_scheduled_source(bg::GraphRunOptions{.engine = bg::EvaluationEngine::realtime,
                                             .start_time = start,
                                             .stop_time = time(13)});

    CHECK(ScheduledSource::evaluation_times == std::vector{start, time(11), time(12)});
    CHECK(ScheduledSource::stop_time == time(12));
}

TEST_CASE("graph run options reject invalid and overflowing limits before startup",
          "[evaluation][simulation][feature:simulation][failure]") {
    auto start_count = std::size_t{};
    const auto source = bg::node([&] { ++start_count; return bg::i64{1}; });
    const auto run = [&](const bg::GraphRunOptions &options) {
        bg::run_graph([&] { (void)source(); }, options);
    };

    CHECK_THROWS_AS(run(bg::GraphRunOptions{.start_time = time(10), .stop_time = time(10)}), std::invalid_argument);
    CHECK_THROWS_AS(run(bg::GraphRunOptions{.run_time = std::chrono::microseconds{0}}), std::invalid_argument);
    CHECK_THROWS_AS(run(bg::GraphRunOptions{.run_time = std::chrono::microseconds{-1}}), std::invalid_argument);
    CHECK_THROWS_AS(run(bg::GraphRunOptions{.start_time = bg::EvaluationTime::max() -
                                                        std::chrono::microseconds{1},
                                            .run_time = std::chrono::microseconds{2}}),
                    std::overflow_error);
    CHECK(start_count == 0);
}

TEST_CASE("evaluation engines reject a stop time at the start before starting the graph",
          "[evaluation][simulation][realtime][lifecycle][failure][feature:simulation][feature:realtime]"
          "[feature:lifecycle]") {
    ScheduledSource::reset();
    const auto source = bg::node<ScheduledSource>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    const auto start = time(50);

    CHECK_THROWS_AS(bg::SimulationEvaluationEngine{}.run(runtime, start, {}, start), std::invalid_argument);
    CHECK_THROWS_AS(bg::RealtimeEvaluationEngine{}.run(runtime, start, std::stop_token{}, {}, start),
                    std::invalid_argument);
    CHECK(ScheduledSource::evaluation_times.empty());
    CHECK_FALSE(ScheduledSource::stop_time.has_value());
}
