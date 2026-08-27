#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

namespace {

struct constant_definition {
    float value;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(value); }
};

struct add_definition {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(lhs.value() + rhs.value());
    }

    inline static int evaluation_count{};
};

struct double_source_definition {
    bg::Out<bg::TS<double>> output;

    void evaluate() const { output.set(1.5); }
};

struct double_sink_definition {
    bg::In<bg::TS<double>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = input.value();
    }

    inline static int evaluation_count{};
    inline static double last_value{};
};

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = input.value();
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

struct counter_state {
    int value{};
};

struct alignas(16) accumulated_state {
    int value{};
};

struct stateful_scalar_definition {
    int factor;
    std::optional<int> increment;
    bg::State<counter_state> state;
    bg::State<accumulated_state> accumulated;
    bg::Out<bg::TS<int>> output;

    void evaluate() const {
        state->value += increment.value_or(1);
        accumulated->value += state->value;
        output.set(accumulated->value * factor);
    }
};

struct int_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { last_value = input.value(); }

    inline static int last_value{};
};

inline const auto constant = bg::node<constant_definition>();
inline const auto add = bg::node<add_definition>();
inline const auto double_source = bg::node<double_source_definition>();
inline const auto double_sink = bg::node<double_sink_definition>();
inline const auto float_sink = bg::node<float_sink_definition>();
inline const auto stateful_scalar = bg::node<stateful_scalar_definition>();
inline const auto int_sink = bg::node<int_sink_definition>();

void wire_add(bg::GraphWiring &wiring) {
    bg::WiringContext context{wiring};
    (void)add(constant(1.0F), constant(2.0F));
}

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("runtime storage uses the output scalar representation", "[evaluation][storage]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        double_sink(double_source());
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};

    double_sink_definition::evaluation_count = 0;
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    CHECK(double_sink_definition::evaluation_count == 1);
    CHECK(double_sink_definition::last_value == 1.5);
}

TEST_CASE("a node without output uses no output storage", "[evaluation][storage]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(constant(1.0F));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    float_sink_definition::evaluation_count = 0;

    CHECK_FALSE(compiled.node(1).has_output());
    CHECK(compiled.runtime_storage_size() == 36);

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 1.0F);
}

TEST_CASE("runtime graphs keep separate runtime storage", "[evaluation][storage]") {
    bg::GraphWiring wiring;
    wire_add(wiring);
    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph first{compiled};

    first.start(time(1));
    first.schedule(0);
    first.schedule(1);
    first.evaluate(time(1));

    bg::RuntimeGraph second{compiled};
    add_definition::evaluation_count = 0;
    second.start(time(1));
    second.schedule(2);
    second.evaluate(time(1));

    CHECK(add_definition::evaluation_count == 0);
}

TEST_CASE("scalar fields and state bind to their storage", "[evaluation][storage]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        int_sink(stateful_scalar(2, std::optional<int>{}));
    }

    const bg::CompiledGraph compiled{wiring};
    const auto &node = compiled.node(0);

    REQUIRE(node.parameters().size() == 2);
    CHECK(*static_cast<const int *>(node.parameters()[0].scalar_data()) == 2);
    CHECK(node.parameters()[1].scalar_data() == nullptr);
    CHECK(node.has_state());

    bg::RuntimeGraph first{compiled};
    first.start(time(1));
    first.schedule(0);
    first.evaluate(time(1));
    CHECK(int_sink_definition::last_value == 2);

    first.schedule(0);
    first.evaluate(time(2));
    CHECK(int_sink_definition::last_value == 6);

    bg::RuntimeGraph second{compiled};
    second.start(time(1));
    second.schedule(0);
    second.evaluate(time(1));
    CHECK(int_sink_definition::last_value == 2);
}
