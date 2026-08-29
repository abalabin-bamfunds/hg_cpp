#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <vector>

namespace {

struct starting_source_definition {
    float value;
    bg::Out<bg::TS<float>> output;

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
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        last_result = lhs.value() + rhs.value();
        output.set(last_result);
    }

    inline static int evaluation_count{};
    inline static float last_result{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

TEST_CASE("run_graph starts, evaluates, and stops a graph", "[evaluation][lifecycle]") {
    const auto starting_source = bg::node<starting_source_definition>();
    const auto add = bg::node<add_definition>();
    starting_source_definition::start_count = 0;
    starting_source_definition::evaluation_count = 0;
    starting_source_definition::stop_count = 0;
    add_definition::evaluation_count = 0;

    bg::run_graph([&] { (void)add(starting_source(1.0F), starting_source(2.0F)); });

    CHECK(starting_source_definition::start_count == 2);
    CHECK(starting_source_definition::evaluation_count == 0);
    CHECK(starting_source_definition::stop_count == 2);
    CHECK(add_definition::evaluation_count == 1);
    CHECK(add_definition::last_result == 3.0F);
}

struct lifecycle_definition {
    int id;

    void start() const {
        events.push_back(id);
        if (id == start_failure) {
            throw std::runtime_error{"start failure"};
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
};

TEST_CASE("start and stop follow topological order", "[evaluation][lifecycle]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::events.reserve(6);
    lifecycle_definition::start_failure = 0;
    lifecycle_definition::evaluation_failure = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        lifecycle(1);
        lifecycle(2);
        lifecycle(3);
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.stop();

    CHECK(lifecycle_definition::events == std::vector{1, 2, 3, -3, -2, -1});
}

TEST_CASE("a start failure stops nodes whose start completed", "[evaluation][lifecycle][failure]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::events.reserve(4);
    lifecycle_definition::start_failure = 2;
    lifecycle_definition::evaluation_failure = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        lifecycle(1);
        lifecycle(2);
        lifecycle(3);
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};

    CHECK_THROWS_AS(runtime.start(time(1)), std::runtime_error);
    CHECK(lifecycle_definition::events == std::vector{1, 2, -1});
}

TEST_CASE("an evaluation failure stops nodes in reverse topological order", "[evaluation][lifecycle][failure]") {
    const auto lifecycle = bg::node<lifecycle_definition>();
    lifecycle_definition::events.clear();
    lifecycle_definition::events.reserve(8);
    lifecycle_definition::start_failure = 0;
    lifecycle_definition::evaluation_failure = 2;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        lifecycle(1);
        lifecycle(2);
        lifecycle(3);
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(1);

    CHECK_THROWS_AS(runtime.evaluate(time(1)), std::runtime_error);
    CHECK(lifecycle_definition::events == std::vector{1, 2, 3, 102, -3, -2, -1});
}

struct immediate_definition {
    void start(bg::EvaluationContext *context) const {
        start_time = context->evaluation_time();
        context->schedule_now();
    }

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluation_count;
        evaluation_time = context->evaluation_time();
    }

    inline static bg::EvaluationTime start_time{bg::MIN_DT};
    inline static bg::EvaluationTime evaluation_time{bg::MIN_DT};
    inline static int evaluation_count{};
};

TEST_CASE("start can schedule evaluation in the first outer cycle", "[evaluation][lifecycle][scheduling]") {
    const auto immediate = bg::node<immediate_definition>();
    immediate_definition::start_time = bg::MIN_DT;
    immediate_definition::evaluation_time = bg::MIN_DT;
    immediate_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        immediate();
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(immediate_definition::evaluation_count == 1);
    CHECK(immediate_definition::start_time == time(1));
    CHECK(immediate_definition::evaluation_time == time(1));
}

} // namespace
