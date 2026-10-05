#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <stdexcept>

namespace {

struct constant_definition {
    float value;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(value); }
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

namespace {

struct add_definition {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        last_result = *lhs + *rhs;
        output.set(last_result);
    }

    inline static int evaluation_count{};
    inline static float last_result{};
};

} // namespace

TEST_CASE("a runtime graph evaluates scheduled nodes in topological order",
          "[evaluation][scheduling][feature:scheduling][shape:chain][series:value]"
          "[scalar:floating-point]") {
    const auto constant = bg::node<constant_definition>();
    const auto add = bg::node<add_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)add(constant(1.0F), constant(2.0F));
    }

    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    add_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    CHECK(add_definition::evaluation_count == 1);
    CHECK(add_definition::last_result == 3.0F);

    runtime.evaluate(time(2));
    CHECK(add_definition::evaluation_count == 1);

    runtime.schedule(0);
    runtime.evaluate(time(3));
    CHECK(add_definition::evaluation_count == 2);
}

namespace {

struct start_only_definition {
    bg::Out<bg::TS<float>> output;

    void start() const { output.set(3.0F); }
};

struct start_only_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = *input;
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

struct initially_scheduled_source_definition {
    static constexpr bool schedule_on_start = true;

    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(7.0F);
    }

    inline static int evaluation_count{};
};

struct initially_scheduled_scalar_definition {
    static constexpr bool schedule_on_start = true;

    int value;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(static_cast<float>(value));
    }

    inline static int evaluation_count{};
};

struct inert_scalar_definition {
    int value;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct initial_value_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const { last_value = *input; }

    inline static float last_value{};
};

} // namespace

TEST_CASE("a scheduled node can omit evaluation",
          "[evaluation][scheduling][feature:scheduling][series:value][scalar:floating-point]") {
    const auto source = bg::node<start_only_definition>();
    const auto sink = bg::node<start_only_sink_definition>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        sink(source());
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    start_only_sink_definition::evaluation_count = 0;
    start_only_sink_definition::last_value = 0.0F;

    CHECK(compiled.node(0).evaluation_function() == nullptr);

    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(start_only_sink_definition::evaluation_count == 1);
    CHECK(start_only_sink_definition::last_value == 3.0F);

    runtime.schedule(0);
    runtime.evaluate(time(2));
    CHECK(start_only_sink_definition::evaluation_count == 1);
}

TEST_CASE("a node can request evaluation in the first cycle",
          "[evaluation][scheduling][feature:scheduling][shape:source][series:value][scalar:floating-point]") {
    const auto source = bg::node<initially_scheduled_source_definition>();
    const auto scalar_source = bg::node<initially_scheduled_scalar_definition>();
    auto callable_evaluation_count = 0;
    const auto callable_source = bg::node(
        bg::eval =
            [&](bg::Out<bg::TS<float>> output) {
                ++callable_evaluation_count;
                output.set(13.0F);
            },
        bg::schedule_on_start = true);
    const auto sink = bg::node<initial_value_sink_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        sink(source());
        sink(scalar_source(11));
        sink(callable_source());
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    initially_scheduled_source_definition::evaluation_count = 0;
    initially_scheduled_scalar_definition::evaluation_count = 0;
    initial_value_sink_definition::last_value = 0.0F;

    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(initially_scheduled_source_definition::evaluation_count == 1);
    CHECK(initially_scheduled_scalar_definition::evaluation_count == 1);
    CHECK(callable_evaluation_count == 1);
    CHECK(initial_value_sink_definition::last_value == 13.0F);
}

TEST_CASE("a scalar-only node is inert unless it requests initial evaluation",
          "[evaluation][scheduling][feature:scheduling][scalar:floating-point]") {
    const auto inert = bg::node<inert_scalar_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        inert(3);
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    inert_scalar_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(inert_scalar_definition::evaluation_count == 0);

    runtime.schedule(0);
    runtime.evaluate(time(2));
    CHECK(inert_scalar_definition::evaluation_count == 1);
}

namespace {

struct indexed_definition {
    int index;

    void evaluate() const { evaluation_order[evaluation_count++] = index; }

    inline static std::array<int, 130> evaluation_order{};
    inline static std::size_t evaluation_count{};
};

} // namespace

TEST_CASE("a runtime graph processes set scheduling bits in node order",
          "[evaluation][scheduling][feature:scheduling][series:value][scalar:floating-point]") {
    const auto indexed = bg::node<indexed_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        for (auto i = 0; i < 130; ++i) {
            indexed(i);
        }
    }

    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    indexed_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(129);
    runtime.schedule(2);
    runtime.schedule(65);
    runtime.evaluate(time(1));

    REQUIRE(indexed_definition::evaluation_count == 3);
    CHECK(indexed_definition::evaluation_order[0] == 2);
    CHECK(indexed_definition::evaluation_order[1] == 65);
    CHECK(indexed_definition::evaluation_order[2] == 129);
}

namespace {

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = *input;
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

} // namespace

TEST_CASE("scheduling during evaluation crosses a scheduling block boundary",
          "[evaluation][scheduling][feature:scheduling][series:value][scalar:floating-point]") {
    const auto constant = bg::node<constant_definition>();
    const auto indexed = bg::node<indexed_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        for (auto i = 0; i < 63; ++i) {
            indexed(i);
        }

        float_sink(constant(1.0F));
    }

    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    float_sink_definition::evaluation_count = 0;
    float_sink_definition::last_value = 0.0F;

    runtime.start(time(1));
    runtime.schedule(63);
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 1.0F);
}

TEST_CASE("evaluation time must increase between outer cycles",
          "[evaluation][scheduling][feature:scheduling][series:value][scalar:floating-point]") {
    const auto indexed = bg::node<indexed_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        indexed(0);
    }

    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK_THROWS_AS(runtime.evaluate(time(1)), std::invalid_argument);
    CHECK_THROWS_AS(runtime.evaluate(bg::MIN_DT), std::invalid_argument);
}
