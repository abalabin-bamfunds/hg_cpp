#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace {

struct one_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(1.0F); }
};

struct two_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(2.0F); }
};

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

struct list_source_definition {
    bg::List<float> values;
    bg::Out<bg::TS<float>> output;

    void start() const { output.set(values[0]); }

    void evaluate() const {}
};

struct generic_list_source_definition {
    bg::Scalar<bg::List<bg::SCALAR>> values;
    bg::Out<bg::TS<bg::SCALAR>> output;

    void start() const { output.set_value(values[0]); }

    void evaluate() const {}
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

struct switchable_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        if (publish) {
            output.set(4.0F);
        } else {
            output.invalidate();
        }
    }

    inline static bool publish{true};
};

struct copy_definition {
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(input.value()); }
};

struct generic_value_copy_definition {
    bg::In<bg::TS<bg::SCALAR>> input;
    bg::Out<bg::TS<bg::SCALAR>> output;

    void evaluate() const { output.set_value(input.value_view()); }
};

struct generic_copy_definition {
    bg::In<bg::TS_T> input;
    bg::Out<bg::TS_T> output;

    void evaluate() const { output.set_value(input.value_view()); }
};

struct passive_copy_definition {
    bg::In<bg::TS<float>, bg::passive> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(input.value());
    }

    inline static int evaluation_count{};
};

struct hold_definition {
    static constexpr auto on_invalidation = bg::preserve_output;

    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(input.value());
    }

    inline static int evaluation_count{};
};

struct fallback_definition {
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(input.value());
    }

    void on_invalidation() const {
        ++invalidation_count;
        output.set(-1.0F);
    }

    inline static int evaluation_count{};
    inline static int invalidation_count{};
};

struct double_source_definition {
    bg::Out<bg::TS<double>> output;

    void evaluate() const { output.set(1.5); }
};

struct double_sink_definition {
    bg::In<bg::TS<double>> input;
    bg::Out<bg::TS<double>> output;

    void evaluate() const {
        ++evaluation_count;
        last_value = input.value();
        output.set(last_value);
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

struct validity_observer_definition {
    bg::In<bg::TS<float>, bg::optional> input;

    void evaluate() const {
        ++evaluation_count;
        last_valid = input.valid();
    }

    inline static int evaluation_count{};
    inline static bool last_valid{};
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

struct indexed_definition {
    int index;

    void evaluate() const { evaluation_order[evaluation_count++] = index; }

    inline static std::array<int, 130> evaluation_order{};
    inline static std::size_t evaluation_count{};
};

inline const auto one = bg::node<one_definition>();
inline const auto two = bg::node<two_definition>();
inline const auto starting_source = bg::node<starting_source_definition>();
inline const auto list_source = bg::node<list_source_definition>();
inline const auto generic_list_source = bg::node<generic_list_source_definition>();
inline const auto add = bg::node<add_definition>();
inline const auto switchable = bg::node<switchable_definition>();
inline const auto copy = bg::node<copy_definition>();
inline const auto generic_value_copy = bg::node<generic_value_copy_definition>();
inline const auto generic_copy = bg::node<generic_copy_definition>();
inline const auto passive_copy = bg::node<passive_copy_definition>();
inline const auto hold = bg::node<hold_definition>();
inline const auto fallback = bg::node<fallback_definition>();
inline const auto double_source = bg::node<double_source_definition>();
inline const auto double_sink = bg::node<double_sink_definition>();
inline const auto float_sink = bg::node<float_sink_definition>();
inline const auto validity_observer = bg::node<validity_observer_definition>();
inline const auto stateful_scalar = bg::node<stateful_scalar_definition>();
inline const auto int_sink = bg::node<int_sink_definition>();
inline const auto indexed = bg::node<indexed_definition>();

void wire_add(bg::GraphWiring &wiring) {
    bg::WiringContext context{wiring};
    const bg::WiringPoint lhs = one();
    const bg::WiringPoint rhs = two();
    (void)add(lhs, rhs);
}

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("run_graph wires, compiles, and evaluates a graph function", "[runtime_graph]") {
    add_definition::evaluation_count = 0;
    starting_source_definition::start_count = 0;
    starting_source_definition::evaluation_count = 0;
    starting_source_definition::stop_count = 0;

    bg::run_graph([] { (void)add(starting_source(1.0F), starting_source(2.0F)); });

    CHECK(starting_source_definition::start_count == 2);
    CHECK(starting_source_definition::evaluation_count == 0);
    CHECK(starting_source_definition::stop_count == 2);
    CHECK(add_definition::evaluation_count == 1);
    CHECK(add_definition::last_result == 3.0F);
}

TEST_CASE("a scalar list passes values through normal wiring", "[runtime_graph]") {
    float_sink_definition::evaluation_count = 0;
    auto values = std::vector{2.5F};
    const auto list = bg::ScalarValue<bg::List<float>>{values};

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        float_sink(list_source(list));
    }
    values[0] = 4.0F;

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

TEST_CASE("a generic scalar list resolves its item view", "[runtime_graph]") {
    float_sink_definition::evaluation_count = 0;
    const auto values = std::vector{2.5F};
    const auto list = bg::ScalarValue<bg::List<float>>{values};

    bg::run_graph([&] { float_sink(generic_list_source(list)); });

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

TEST_CASE("compiled generic value views preserve the resolved scalar type", "[runtime_graph]") {
    float_sink_definition::evaluation_count = 0;

    bg::run_graph([] { float_sink(generic_value_copy(starting_source(2.5F))); });

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

TEST_CASE("compiled arbitrary time-series views preserve the resolved time-series type", "[runtime_graph]") {
    double_sink_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        (void)double_sink(generic_copy(double_source()));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    CHECK(double_sink_definition::evaluation_count == 1);
    CHECK(double_sink_definition::last_value == 1.5);
}

TEST_CASE("a runtime graph evaluates scheduled nodes in topological order", "[runtime_graph]") {
    bg::GraphWiring wiring;
    wire_add(wiring);
    bg::CompiledGraph compiled{wiring};
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

TEST_CASE("a runtime graph processes set schedule bits", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        for (int i = 0; i < 130; ++i) {
            indexed(i);
        }
    }

    bg::CompiledGraph compiled{wiring};
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

TEST_CASE("runtime storage uses the output scalar representation", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)double_sink(double_source());
    }

    bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};

    double_sink_definition::evaluation_count = 0;
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    CHECK(double_sink_definition::evaluation_count == 1);
    CHECK(double_sink_definition::last_value == 1.5);
}

TEST_CASE("a node without output uses no output storage", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(one());
    }

    bg::CompiledGraph compiled{wiring};
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

TEST_CASE("a required input delays evaluation until it becomes valid", "[runtime_graph]") {
    bg::GraphWiring wiring;
    wire_add(wiring);
    bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    add_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    CHECK(add_definition::evaluation_count == 0);

    runtime.schedule(1);
    runtime.evaluate(time(2));

    CHECK(add_definition::evaluation_count == 1);
    CHECK(add_definition::last_result == 3.0F);
}

TEST_CASE("runtime graphs keep separate state", "[runtime_graph]") {
    bg::GraphWiring wiring;
    wire_add(wiring);
    bg::CompiledGraph compiled{wiring};
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

TEST_CASE("aggregate scalar fields and state bind to their storage", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        int_sink(stateful_scalar(2, std::optional<int>{}));
    }

    bg::CompiledGraph compiled{wiring};
    const bg::CompiledNode &node = compiled.node(0);

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

TEST_CASE("required-input invalidation propagates through the graph", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(copy(switchable()));
    }

    bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    float_sink_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));
    REQUIRE(float_sink_definition::evaluation_count == 1);

    switchable_definition::publish = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    CHECK(float_sink_definition::evaluation_count == 1);
}

TEST_CASE("passive required inputs propagate validity without evaluation", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        validity_observer(passive_copy(switchable()));
    }

    bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    passive_copy_definition::evaluation_count = 0;
    validity_observer_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    REQUIRE(passive_copy_definition::evaluation_count == 1);
    REQUIRE(validity_observer_definition::evaluation_count == 1);
    REQUIRE(validity_observer_definition::last_valid);

    switchable_definition::publish = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    CHECK(passive_copy_definition::evaluation_count == 1);
    CHECK(validity_observer_definition::evaluation_count == 2);
    CHECK_FALSE(validity_observer_definition::last_valid);

    switchable_definition::publish = true;
    runtime.schedule(0);
    runtime.evaluate(time(3));

    CHECK(passive_copy_definition::evaluation_count == 1);
    CHECK(validity_observer_definition::evaluation_count == 2);

    runtime.schedule(1);
    runtime.evaluate(time(4));

    CHECK(passive_copy_definition::evaluation_count == 2);
    CHECK(validity_observer_definition::evaluation_count == 3);
    CHECK(validity_observer_definition::last_valid);

    runtime.schedule(0);
    runtime.evaluate(time(5));

    CHECK(passive_copy_definition::evaluation_count == 2);
    CHECK(validity_observer_definition::evaluation_count == 3);
}

TEST_CASE("a node can preserve its output when a required input becomes invalid", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        validity_observer(hold(switchable()));
    }

    bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    hold_definition::evaluation_count = 0;
    validity_observer_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    REQUIRE(hold_definition::evaluation_count == 1);
    REQUIRE(validity_observer_definition::evaluation_count == 1);
    REQUIRE(validity_observer_definition::last_valid);

    switchable_definition::publish = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    CHECK(hold_definition::evaluation_count == 1);
    CHECK(validity_observer_definition::evaluation_count == 1);
}

TEST_CASE("a node can produce an output when a required input becomes invalid", "[runtime_graph]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(fallback(switchable()));
    }

    bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    switchable_definition::publish = true;
    fallback_definition::evaluation_count = 0;
    fallback_definition::invalidation_count = 0;
    float_sink_definition::evaluation_count = 0;

    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    REQUIRE(fallback_definition::evaluation_count == 1);
    REQUIRE(fallback_definition::invalidation_count == 0);
    REQUIRE(float_sink_definition::last_value == 4.0F);

    switchable_definition::publish = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    CHECK(fallback_definition::evaluation_count == 1);
    CHECK(fallback_definition::invalidation_count == 1);
    CHECK(float_sink_definition::evaluation_count == 2);
    CHECK(float_sink_definition::last_value == -1.0F);
}
