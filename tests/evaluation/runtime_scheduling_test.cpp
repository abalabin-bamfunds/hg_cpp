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

struct indexed_definition {
    int index;

    void evaluate() const { evaluation_order[evaluation_count++] = index; }

    inline static std::array<int, 130> evaluation_order{};
    inline static std::size_t evaluation_count{};
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
        last_result = lhs.value() + rhs.value();
        output.set(last_result);
    }

    inline static int evaluation_count{};
    inline static float last_result{};
};

} // namespace

TEST_CASE("a runtime graph evaluates scheduled nodes in topological order", "[evaluation][scheduling]") {
    const auto constant = bg::node<constant_definition>();
    const auto add = bg::node<add_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)add(constant(1.0F), constant(2.0F));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
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

TEST_CASE("a runtime graph processes set scheduling bits in node order", "[evaluation][scheduling]") {
    const auto indexed = bg::node<indexed_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        for (auto i = 0; i < 130; ++i) {
            indexed(i);
        }
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
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
        last_value = input.value();
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

} // namespace

TEST_CASE("scheduling during evaluation crosses a scheduling block boundary", "[evaluation][scheduling]") {
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
    bg::RuntimeGraph runtime{compiled};
    float_sink_definition::evaluation_count = 0;
    float_sink_definition::last_value = 0.0F;

    runtime.start(time(1));
    runtime.schedule(63);
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 1.0F);
}

TEST_CASE("evaluation time must increase between outer cycles", "[evaluation][scheduling]") {
    const auto indexed = bg::node<indexed_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        indexed(0);
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK_THROWS_AS(runtime.evaluate(time(1)), std::invalid_argument);
    CHECK_THROWS_AS(runtime.evaluate(bg::MIN_DT), std::invalid_argument);
}
