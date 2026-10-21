#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/wall_clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stop_token>
#include <stdexcept>
#include <vector>

using namespace bg;

namespace {

struct starting_source_definition {
    float value;
    Out<TS<float>> output;

    void start() const {
        ++start_count;
        output.set(value);
    }

    void evaluate() const { ++evaluation_count; }

    void stop() const noexcept { ++stop_count; }

    inline static int start_count{};
    inline static int evaluation_count{};
    inline static int stop_count{};
};

struct add_definition {
    In<TS<float>> lhs;
    In<TS<float>> rhs;
    Out<TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        last_result = *lhs + *rhs;
        output.set(last_result);
    }

    inline static int evaluation_count{};
    inline static float last_result{};
};

struct run_graph_start_time_probe {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        observed_time = context->evaluation_time();
        output.set(1);
    }

    inline static EvaluationTime observed_time{};
};

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

TEST_CASE("run_graph starts, evaluates, and stops a graph",
          "[evaluation][lifecycle][feature:lifecycle][transition:start][transition:stop][series:value]"
          "[scalar:floating-point]") {
    const auto starting_source = node<starting_source_definition>();

    const auto add = node<add_definition>();
    starting_source_definition::start_count = 0;
    starting_source_definition::evaluation_count = 0;
    starting_source_definition::stop_count = 0;
    add_definition::evaluation_count = 0;

    run_graph([&] { (void)add(starting_source(1.0F), starting_source(2.0F)); });

    CHECK(starting_source_definition::start_count == 2);
    CHECK(starting_source_definition::evaluation_count == 0);
    CHECK(starting_source_definition::stop_count == 2);
    CHECK(add_definition::evaluation_count == 1);
    CHECK(add_definition::last_result == 3.0F);
}

TEST_CASE("run_graph uses an explicit simulation start time",
          "[evaluation][lifecycle][feature:simulation][transition:start]") {
    run_graph_start_time_probe::observed_time = {};

    const auto source = node<run_graph_start_time_probe>();
    const auto start_time = EvaluationTime{std::chrono::microseconds{123}};

    run_graph([&] { (void)source(); },
                  GraphRunOptions{.engine = EvaluationEngine::simulation, .start_time = start_time});

    CHECK(run_graph_start_time_probe::observed_time == start_time);
}

TEST_CASE("run_graph forwards stop tokens to the realtime engine",
          "[evaluation][lifecycle][feature:realtime][transition:stop]") {
    auto starts = 0;

    const auto source = node([&](Out<TS<i64>>) { ++starts; });
    auto stop_source = std::stop_source{};
    stop_source.request_stop();

    run_graph([&] { (void)source(); },
                  GraphRunOptions{.engine = EvaluationEngine::realtime,
                                      .stop_token = stop_source.get_token()});

    CHECK(starts == 0);
}

TEST_CASE("run_graph ignores stop tokens in simulation", "[evaluation][lifecycle][feature:simulation]") {
    auto stop_source = std::stop_source{};
    stop_source.request_stop();
    run_graph_start_time_probe::observed_time = {};

    const auto source = node<run_graph_start_time_probe>();

    run_graph([&] { (void)source(); }, GraphRunOptions{.stop_token = stop_source.get_token()});

    CHECK(run_graph_start_time_probe::observed_time == MIN_DT + std::chrono::microseconds{1});
}

namespace {
struct run_graph_time_sink {
    In<TS<i64>> input;

    void evaluate(EvaluationContext *context) const {
        observed_time = context->evaluation_time();
        ++calls;
    }

    inline static EvaluationTime observed_time{};
    inline static int calls{};
};
} // namespace

TEST_CASE("run_graph stops realtime evaluation after a complete downstream tick",
          "[evaluation][lifecycle][feature:realtime][transition:stop][shape:chain]") {
    auto stop_source = std::stop_source{};
    run_graph_time_sink::calls = 0;
    run_graph_time_sink::observed_time = {};
    auto wired_at = EvaluationTime{};

    const auto source = node([&](Out<TS<i64>> output) {
        output.set(7);
        stop_source.request_stop();
    });

    const auto sink = node<run_graph_time_sink>();

    run_graph(
        [&] {
            (void)sink(source());
            wired_at = SystemWallClock::instance().now();
        },
        GraphRunOptions{.engine = EvaluationEngine::realtime, .stop_token = stop_source.get_token()});

    CHECK(run_graph_time_sink::calls == 1);
    CHECK(run_graph_time_sink::observed_time >= wired_at);
}

struct lifecycle_definition {
    int id;

    void start(EvaluationContext *context) const {
        events.push_back(id);
        if (id == start_failure) {
            throw std::runtime_error{"start failure"};
        }
        if (id == scheduled_evaluation) {
            context->schedule_now();
        }
    }

    void evaluate() const {
        events.push_back(100 + id);
        if (id == evaluation_failure) {
            throw std::runtime_error{"evaluation failure"};
        }
    }

    void stop() const noexcept { events.push_back(-id); }

    inline static std::vector<int> events;
    inline static int start_failure{};
    inline static int evaluation_failure{};
    inline static int scheduled_evaluation{};
};

TEST_CASE("start and stop follow topological order",
          "[evaluation][lifecycle][feature:lifecycle][shape:chain][transition:stop][series:value]"
          "[scalar:floating-point]") {
    const auto lifecycle = node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::events.reserve(6);
    lifecycle_definition::start_failure = 0;
    lifecycle_definition::evaluation_failure = 0;
    lifecycle_definition::scheduled_evaluation = 0;

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        lifecycle(1);
        lifecycle(2);
        lifecycle(3);
    }

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.stop();

    CHECK(lifecycle_definition::events == std::vector{1, 2, 3, -3, -2, -1});
}

TEST_CASE("a start failure stops nodes whose start completed",
          "[evaluation][lifecycle][failure][feature:lifecycle][transition:start][transition:stop]"
          "[series:value][scalar:floating-point]") {
    const auto lifecycle = node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::events.reserve(4);
    lifecycle_definition::start_failure = 2;
    lifecycle_definition::evaluation_failure = 0;
    lifecycle_definition::scheduled_evaluation = 0;

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        lifecycle(1);
        lifecycle(2);
        lifecycle(3);
    }

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{compiled, runtime_global_state};

    CHECK_THROWS_AS(runtime.start(time(1)), std::runtime_error);
    CHECK(lifecycle_definition::events == std::vector{1, 2, -1});
}

TEST_CASE("an evaluation failure stops nodes in reverse topological order",
          "[evaluation][lifecycle][failure][feature:lifecycle][shape:chain][transition:stop]"
          "[series:value][scalar:floating-point]") {
    const auto lifecycle = node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::events.reserve(8);
    lifecycle_definition::start_failure = 0;
    lifecycle_definition::evaluation_failure = 2;
    lifecycle_definition::scheduled_evaluation = 2;

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        lifecycle(1);
        lifecycle(2);
        lifecycle(3);
    }

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));

    CHECK_THROWS_AS(runtime.evaluate(time(1)), std::runtime_error);
    CHECK(lifecycle_definition::events == std::vector{1, 2, 3, 102, -3, -2, -1});
}

TEST_CASE("a runtime graph has one lifecycle",
          "[evaluation][lifecycle][feature:lifecycle][transition:restart][series:value]"
          "[scalar:floating-point]") {
    const auto lifecycle = node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::start_failure = 0;
    lifecycle_definition::evaluation_failure = 0;
    lifecycle_definition::scheduled_evaluation = 0;

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        lifecycle(1);
    }

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{compiled, runtime_global_state};

    CHECK_THROWS_AS(runtime.evaluate(time(1)), std::logic_error);
    runtime.start(time(1));
    CHECK_THROWS_AS(runtime.start(time(1)), std::logic_error);
    runtime.stop();
    runtime.stop();
    CHECK_THROWS_AS(runtime.evaluate(time(2)), std::logic_error);
}

TEST_CASE("an aggregate node finalizes retained state when its runtime graph is destroyed",
          "[evaluation][lifecycle][feature:lifecycle][feature:finalization][definition:aggregate]") {
    struct Finalizable {
        Scalar<Opaque<int *>> count;
        State<int> retained_state;

        void start() const { ++*retained_state; }
        void finalize() const noexcept { ++*count.value(); }
    };

    auto finalization_count = 0;

    const auto finalizable = node<Finalizable>();
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        finalizable(ScalarValue<Opaque<int *>>{&finalization_count});
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    {
        auto runtime_global_state = GlobalState{};
        auto runtime = RuntimeGraph{compiled, runtime_global_state};
        runtime.start(time(1));
    }

    CHECK(finalization_count == 1);
}

struct immediate_definition {
    void start(EvaluationContext *context) const {
        start_time = context->evaluation_time();
        context->schedule_now();
    }

    void evaluate(EvaluationContext *context) const {
        ++evaluation_count;
        evaluation_time = context->evaluation_time();
    }

    inline static EvaluationTime start_time{MIN_DT};
    inline static EvaluationTime evaluation_time{MIN_DT};
    inline static int evaluation_count{};
};

TEST_CASE("start can schedule evaluation in the first outer cycle",
          "[evaluation][lifecycle][scheduling][feature:lifecycle][series:value][scalar:floating-point]") {
    const auto immediate = node<immediate_definition>();
    immediate_definition::start_time = MIN_DT;
    immediate_definition::evaluation_time = MIN_DT;
    immediate_definition::evaluation_count = 0;

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        immediate();
    }

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(immediate_definition::evaluation_count == 1);
    CHECK(immediate_definition::start_time == time(1));
    CHECK(immediate_definition::evaluation_time == time(1));
}

} // namespace
