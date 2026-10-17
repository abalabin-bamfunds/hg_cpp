#include <bg/evaluation_profiler.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/wall_clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {

bg::EvaluationTime minute(bg::i64 index) {
    return bg::EvaluationTime{std::chrono::minutes{index}};
}

bg::EvaluationProfileCycle cycle(bg::GraphInstanceId graph_id, bg::EvaluationProfileCycle::Boundary boundary,
                                 bg::EvaluationTime wall_time, std::uint64_t checkpoint,
                                 bg::i64 steady_nanoseconds = 0) {
    return {graph_id, boundary, wall_time, wall_time, checkpoint, steady_nanoseconds};
}

struct profiler_source {
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const { output.set(1); }
};

} // namespace

TEST_CASE("evaluation profiler retains compact node periods and releases stopped node totals",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(1); });
    const auto sink = bg::node([](bg::In<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        sink(source());
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 2);

    auto profiler = bg::EvaluationProfiler{};
    const auto graph_id = bg::GraphInstanceId{9};
    profiler.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});

    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           minute(0) + std::chrono::seconds{10}, 100));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 0, 110});
    profiler.consume(bg::EvaluationProfileNode{graph_id, 1, 120});
    profiler.consume(bg::EvaluationProfileNode{graph_id, 0, 125});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           minute(0) + std::chrono::seconds{11}, 130));

    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           minute(0) + std::chrono::seconds{20}, 200));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 1, 220});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           minute(0) + std::chrono::seconds{21}, 230));

    REQUIRE(profiler.node_totals(graph_id, 0));
    CHECK(profiler.node_totals(graph_id, 0)->evaluations == 2);
    CHECK(profiler.node_totals(graph_id, 0)->checkpoint_ticks == 15);
    CHECK_FALSE(profiler.node_period(graph_id, 0, minute(0)));

    profiler.advance_to(minute(1));
    REQUIRE(profiler.node_period(graph_id, 0, minute(0)));
    CHECK(profiler.node_period(graph_id, 0, minute(0))->evaluations == 2);
    CHECK(profiler.node_period(graph_id, 0, minute(0))->checkpoint_ticks == 15);
    REQUIRE(profiler.node_period(graph_id, 1, minute(0)));
    CHECK(profiler.node_period(graph_id, 1, minute(0))->evaluations == 2);
    CHECK(profiler.node_period(graph_id, 1, minute(0))->checkpoint_ticks == 30);
    REQUIRE(profiler.graph_period(graph_id, minute(0)));
    CHECK(profiler.graph_period(graph_id, minute(0))->evaluations == 2);
    CHECK(profiler.graph_period(graph_id, minute(0))->checkpoint_ticks == 60);

    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           minute(1) + std::chrono::seconds{2}, 300));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 1, 310});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           minute(1) + std::chrono::seconds{3}, 320));
    profiler.consume(bg::EvaluationProfileGraphStop{graph_id});
    CHECK_FALSE(profiler.node_totals(graph_id, 1));
    REQUIRE(profiler.graph(graph_id));
    CHECK_FALSE(profiler.graph(graph_id)->active);

    profiler.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});
    REQUIRE(profiler.graph(graph_id));
    CHECK(profiler.graph(graph_id)->active);
    CHECK(profiler.graph(graph_id)->totals.evaluations == 3);
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           minute(1) + std::chrono::seconds{4}, 400));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 1, 410});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           minute(1) + std::chrono::seconds{5}, 420));
    CHECK(profiler.node_totals(graph_id, 1)->evaluations == 1);
    profiler.consume(bg::EvaluationProfileGraphStop{graph_id});
    profiler.advance_to(minute(2));
    REQUIRE(profiler.node_period(graph_id, 1, minute(1)));
    CHECK(profiler.node_period(graph_id, 1, minute(1))->evaluations == 2);
    CHECK(profiler.node_period(graph_id, 1, minute(1))->checkpoint_ticks == 20);
    const auto nodes = profiler.node_periods(graph_id, minute(1));
    REQUIRE(nodes.size() == 1);
    CHECK(nodes.front().node_index == 1);
    CHECK(nodes.front().totals.evaluations == 2);
    REQUIRE(profiler.graph_period(graph_id, minute(1)));
    CHECK(profiler.graph_period(graph_id, minute(1))->evaluations == 2);
    CHECK(profiler.graph_period(graph_id, minute(1))->checkpoint_ticks == 40);

    profiler.advance_to(minute(30));
    REQUIRE(profiler.node_period(graph_id, 0, minute(0)));
    profiler.advance_to(minute(31));
    CHECK_FALSE(profiler.node_period(graph_id, 0, minute(0)));
    CHECK(profiler.graph(graph_id) != nullptr);
    profiler.advance_to(minute(32));
    CHECK(profiler.graph(graph_id) == nullptr);
}

TEST_CASE("evaluation profiler excludes nested graph work from parent node checkpoints",
          "[evaluation][profile][feature:profiling][shape:nested]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(1); });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};

    auto profiler = bg::EvaluationProfiler{};
    const auto parent = bg::GraphInstanceId{1};
    const auto child = bg::GraphInstanceId{2};
    profiler.consume(bg::EvaluationProfileGraph{parent, &compiled, {}, UINT32_MAX, std::nullopt});
    profiler.consume(bg::EvaluationProfileGraph{child, &compiled, parent, 0, std::nullopt});
    REQUIRE(profiler.graph(child));
    CHECK(profiler.graph(child)->parent_graph_id == parent);

    profiler.consume(cycle(parent, bg::EvaluationProfileCycle::Boundary::start, minute(0), 100));
    profiler.consume(bg::EvaluationProfileGraphEntry{child, 110});
    profiler.consume(bg::EvaluationProfileNode{child, 0, 115});
    profiler.consume(bg::EvaluationProfileGraphExit{child, 120});
    profiler.consume(bg::EvaluationProfileNode{parent, 0, 125});
    profiler.consume(cycle(parent, bg::EvaluationProfileCycle::Boundary::end, minute(0), 130));

    REQUIRE(profiler.node_totals(parent, 0));
    CHECK(profiler.node_totals(parent, 0)->checkpoint_ticks == 15);
    REQUIRE(profiler.node_totals(child, 0));
    CHECK(profiler.node_totals(child, 0)->checkpoint_ticks == 5);
    CHECK(profiler.graph(parent)->totals.checkpoint_ticks == 30);
    CHECK(profiler.graph(child)->totals.checkpoint_ticks == 10);
}

TEST_CASE("evaluation profiler closes and retains configurable periods",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(1); });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};

    auto profiler = bg::EvaluationProfiler{{.node_period = std::chrono::seconds{10},
                                            .retained_node_periods = 2}};
    const auto graph_id = bg::GraphInstanceId{17};
    profiler.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});

    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           bg::EvaluationTime{std::chrono::seconds{9}}, 100));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 0, 105});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           bg::EvaluationTime{std::chrono::seconds{11}}, 110));
    CHECK_FALSE(profiler.node_period(graph_id, 0, bg::EvaluationTime{}));

    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           bg::EvaluationTime{std::chrono::seconds{20}}, 200));
    REQUIRE(profiler.node_period(graph_id, 0, bg::EvaluationTime{}));
    CHECK(profiler.node_period(graph_id, 0, bg::EvaluationTime{})->evaluations == 1);
    CHECK(profiler.node_period(graph_id, 0, bg::EvaluationTime{})->checkpoint_ticks == 5);
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           bg::EvaluationTime{std::chrono::seconds{21}}, 210));
    profiler.consume(bg::EvaluationProfileGraphStop{graph_id});

    CHECK(profiler.node_period(graph_id, 0, bg::EvaluationTime{}));
    profiler.advance_to(bg::EvaluationTime{std::chrono::seconds{30}});
    CHECK_FALSE(profiler.node_period(graph_id, 0, bg::EvaluationTime{}));

    profiler.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           bg::EvaluationTime{std::chrono::seconds{31}}, 300));
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           bg::EvaluationTime{std::chrono::seconds{32}}, 310));
    profiler.consume(bg::EvaluationProfileGraphStop{graph_id});

    profiler.advance_to(bg::EvaluationTime{std::chrono::seconds{40}});
    CHECK(profiler.graph(graph_id) != nullptr);
    profiler.advance_to(bg::EvaluationTime{std::chrono::seconds{50}});
    CHECK(profiler.graph(graph_id) != nullptr);
    profiler.advance_to(bg::EvaluationTime{std::chrono::seconds{60}});
    CHECK(profiler.graph(graph_id) == nullptr);
}

TEST_CASE("evaluation profiler retains fine outer-cycle timing independently of node periods",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(1); });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};

    auto profiler = bg::EvaluationProfiler{{.node_period = std::chrono::minutes{1},
                                            .retained_node_periods = 2,
                                            .outer_period = std::chrono::seconds{1},
                                            .retained_outer_periods = 3}};
    const auto graph_id = bg::GraphInstanceId{23};
    profiler.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});

    const auto first = bg::EvaluationTime{std::chrono::milliseconds{200}};
    const auto second = bg::EvaluationTime{std::chrono::milliseconds{700}};
    const auto third = bg::EvaluationTime{std::chrono::milliseconds{1200}};
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start, first, 100, 1000));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 0, 110});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end, first, 120, 1130));
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start, second, 200, 2000));
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end, second, 220, 2040));
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start, third, 300, 3000));
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end, third, 320, 3090));

    const auto totals = profiler.outer_totals();
    CHECK(totals.cycles == 3);
    CHECK(totals.total_nanoseconds == 260);
    CHECK(totals.min_nanoseconds == 40);
    CHECK(totals.max_nanoseconds == 130);
    REQUIRE(profiler.outer_period(bg::EvaluationTime{}));
    CHECK(profiler.outer_period(bg::EvaluationTime{})->cycles == 2);
    CHECK(profiler.outer_period(bg::EvaluationTime{})->total_nanoseconds == 170);
    CHECK_FALSE(profiler.outer_period(bg::EvaluationTime{std::chrono::seconds{1}}));

    profiler.advance_to(bg::EvaluationTime{std::chrono::seconds{2}});
    const auto periods = profiler.outer_periods();
    REQUIRE(periods.size() == 2);
    CHECK(periods[0].start == bg::EvaluationTime{});
    CHECK(periods[1].start == bg::EvaluationTime{std::chrono::seconds{1}});
    CHECK(periods[1].timing.max_nanoseconds == 90);
    CHECK(profiler.node_period_starts().empty());
    CHECK(profiler.graph_ids() == std::vector<bg::GraphInstanceId>{graph_id});
    CHECK(profiler.report(1).find("Outer graph: 3 cycles") != std::string::npos);
    CHECK(profiler.report(1).find("1970-01-01T00:00:01Z:") != std::string::npos);

    profiler.advance_to(bg::EvaluationTime{std::chrono::seconds{4}});
    CHECK_FALSE(profiler.outer_period(bg::EvaluationTime{}));
    CHECK(profiler.outer_totals().cycles == 3);

    auto fractional = bg::EvaluationProfiler{{.outer_period = std::chrono::milliseconds{500}}};
    fractional.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});
    fractional.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start, second, 100, 1000));
    fractional.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end, second, 110, 1040));
    fractional.advance_to(bg::EvaluationTime{std::chrono::seconds{1}});
    CHECK(fractional.report().find("1970-01-01T00:00:00.500000Z:") != std::string::npos);
}

TEST_CASE("evaluation profiler aligns negative times and validates its configuration",
          "[evaluation][profile][feature:profiling]") {
    CHECK_THROWS_AS(bg::EvaluationProfiler({.node_period = std::chrono::microseconds{0}}), std::invalid_argument);
    CHECK_THROWS_AS(bg::EvaluationProfiler({.retained_node_periods = 0}), std::invalid_argument);
    CHECK_THROWS_AS(bg::EvaluationProfiler({.outer_period = std::chrono::microseconds{0}}), std::invalid_argument);
    CHECK_THROWS_AS(bg::EvaluationProfiler({.retained_outer_periods = 0}), std::invalid_argument);

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(1); });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};

    auto profiler = bg::EvaluationProfiler{{.node_period = std::chrono::seconds{10},
                                            .retained_node_periods = 1}};
    const auto graph_id = bg::GraphInstanceId{18};
    profiler.consume(bg::EvaluationProfileGraph{graph_id, &compiled, {}, UINT32_MAX, std::nullopt});

    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::start,
                           bg::EvaluationTime{std::chrono::seconds{-1}}, 100));
    profiler.consume(bg::EvaluationProfileNode{graph_id, 0, 105});
    profiler.consume(cycle(graph_id, bg::EvaluationProfileCycle::Boundary::end,
                           bg::EvaluationTime{std::chrono::seconds{-1}}, 110));
    profiler.advance_to(bg::EvaluationTime{});
    REQUIRE(profiler.node_period(graph_id, 0, bg::EvaluationTime{std::chrono::seconds{-10}}));
    CHECK(profiler.node_period(graph_id, 0, bg::EvaluationTime{std::chrono::seconds{-10}})->evaluations == 1);
}

TEST_CASE("evaluation profiler consumes committed runtime records",
          "[evaluation][profile][feature:profiling][shape:single-node]") {
    const auto source = bg::node<profiler_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{};
    runtime.start(minute(0) + std::chrono::microseconds{1}, profile);
    runtime.evaluate(minute(0) + std::chrono::microseconds{2}, profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    auto profiler = bg::EvaluationProfiler{};
    CHECK(profiler.poll(reader) > 0);
    CHECK(profiler.poll(reader) == 0);
    REQUIRE(profiler.graph(runtime.instance_id()));
    CHECK(profiler.graph(runtime.instance_id())->totals.evaluations == 1);
    REQUIRE(profiler.graph(runtime.instance_id())->last_updated);
    const auto recorded_minute =
        std::chrono::floor<std::chrono::minutes>(*profiler.graph(runtime.instance_id())->last_updated);
    profiler.advance_to(recorded_minute + std::chrono::minutes{1});
    REQUIRE(profiler.node_period(runtime.instance_id(), 0, recorded_minute));
    CHECK(profiler.node_period(runtime.instance_id(), 0, recorded_minute)->evaluations == 1);
}

TEST_CASE("run_graph records a completed profile", "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profiler_source>();
    auto profiler = bg::EvaluationProfiler{};
    auto global_state = bg::GlobalState{};
    auto services = bg::ServiceCatalogue{};

    bg::run_graph([&] { (void)source(); },
                  bg::GraphRunOptions{.global_state = &global_state, .services = &services,
                                      .recorder = profiler.recorder()});

    const auto root = profiler.root_graph_id();
    REQUIRE(root);
    const auto *graph = profiler.graph(*root);
    REQUIRE(graph);
    CHECK_FALSE(graph->active);
    CHECK(graph->totals.evaluations == 1);
    REQUIRE(graph->last_updated);
    const auto period = std::chrono::floor<std::chrono::minutes>(*graph->last_updated);
    REQUIRE(profiler.graph_period(*root, period));
    CHECK(profiler.graph_period(*root, period)->evaluations == 1);
    REQUIRE(profiler.node_period(*root, 0, period));
    CHECK(profiler.node_period(*root, 0, period)->evaluations == 1);
    CHECK(profiler.outer_totals().cycles == 1);
    REQUIRE(profiler.outer_periods().size() == 1);
    CHECK(profiler.outer_periods().front().timing.cycles == 1);
    CHECK(profiler.graph_ids() == std::vector<bg::GraphInstanceId>{*root});
    REQUIRE(profiler.node_period_starts().size() == 1);
    const auto nodes = profiler.node_periods(*root, period);
    REQUIRE(nodes.size() == 1);
    CHECK(nodes.front().node_index == 0);
    CHECK(nodes.front().totals.evaluations == 1);
    CHECK(profiler.report().find("1 cycle") != std::string::npos);
}

namespace {
struct profiled_realtime_source {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        ++calls;
        observed_time = context->evaluation_time();
        used_global_state = &context->global_state() == global_state;
        output.set(1);
        if (calls == 1) {
            context->schedule_at(context->evaluation_time() + std::chrono::seconds{1});
        }
        stop_source->request_stop();
    }

    inline static int calls{};
    inline static bg::EvaluationTime observed_time{};
    inline static bool used_global_state{};
    inline static bg::GlobalState *global_state{};
    inline static std::stop_source *stop_source{};
};
} // namespace

TEST_CASE("run_graph records realtime evaluation", "[evaluation][feature:profiling][feature:realtime]") {
    auto stop_source = std::stop_source{};
    auto global_state = bg::GlobalState{};
    auto services = bg::ServiceCatalogue{};
    profiled_realtime_source::calls = 0;
    profiled_realtime_source::observed_time = {};
    profiled_realtime_source::used_global_state = false;
    profiled_realtime_source::global_state = &global_state;
    profiled_realtime_source::stop_source = &stop_source;
    const auto source = bg::node<profiled_realtime_source>();
    auto profiler = bg::EvaluationProfiler{};
    const auto start_time = bg::SystemWallClock::instance().now() - std::chrono::seconds{1};

    bg::run_graph([&] { (void)source(); },
                  bg::GraphRunOptions{.global_state = &global_state,
                                      .services = &services,
                                      .recorder = profiler.recorder(),
                                      .engine = bg::EvaluationEngine::realtime,
                                      .start_time = start_time,
                                      .stop_token = stop_source.get_token()});

    const auto root = profiler.root_graph_id();
    REQUIRE(root);
    const auto *graph = profiler.graph(*root);
    REQUIRE(graph);
    CHECK_FALSE(graph->active);
    CHECK(graph->totals.evaluations == 1);
    CHECK(profiled_realtime_source::calls == 1);
    CHECK(profiled_realtime_source::observed_time == start_time);
    CHECK(profiled_realtime_source::used_global_state);
}

TEST_CASE("run_graph joins the profile reader after graph failure", "[evaluation][profile][feature:profiling]") {
    const auto failing = bg::node([](bg::Out<bg::TS<bg::i64>>) { throw std::runtime_error{"graph failure"}; });
    auto profiler = bg::EvaluationProfiler{};

    const auto options = bg::GraphRunOptions{.recorder = profiler.recorder()};
    CHECK_THROWS_AS(bg::run_graph([&] { (void)failing(); }, options), std::runtime_error);
    CHECK_THROWS_AS(bg::run_graph([&] { (void)failing(); }, options), std::logic_error);
}

struct profiler_failing_after_tick {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<std::size_t> calls;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        if ((*calls)++ == 0) {
            output.set(1);
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }
        throw std::runtime_error{"graph failure"};
    }
};

TEST_CASE("run_graph releases compiled metadata after a later-cycle failure",
          "[evaluation][profile][feature:profiling]") {
    const auto failing = bg::node<profiler_failing_after_tick>();
    auto profiler = bg::EvaluationProfiler{};

    CHECK_THROWS_AS(bg::run_graph([&] { (void)failing(); }, bg::GraphRunOptions{.recorder = profiler.recorder()}),
                    std::runtime_error);

    const auto root = profiler.root_graph_id();
    REQUIRE(root);
    const auto *graph = profiler.graph(*root);
    REQUIRE(graph);
    CHECK_FALSE(graph->active);
    CHECK(graph->compiled_graph == nullptr);
    REQUIRE(graph->last_updated);
    const auto period = std::chrono::floor<std::chrono::minutes>(*graph->last_updated);
    REQUIRE(profiler.graph_period(*root, period));
    CHECK(profiler.graph_period(*root, period)->evaluations == 1);
}
