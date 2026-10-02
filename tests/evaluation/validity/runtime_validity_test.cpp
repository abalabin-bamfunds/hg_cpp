#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

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
        last_result = *lhs + *rhs;
        output.set(last_result);
    }

    inline static int evaluation_count{};
    inline static float last_result{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a required input delays evaluation until it becomes valid",
          "[evaluation][validity][feature:validity][validity:required][transition:first-tick]"
          "[series:value][scalar:floating-point]") {
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
    runtime.evaluate(time(1));

    CHECK(add_definition::evaluation_count == 0);

    runtime.schedule(1);
    runtime.evaluate(time(2));

    CHECK(add_definition::evaluation_count == 1);
    CHECK(add_definition::last_result == 3.0F);
}

namespace {

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

    void evaluate() const { output.set(*input); }
};

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

TEST_CASE("revalidating an active required input schedules normal evaluation",
          "[evaluation][validity][feature:validity][validity:required][transition:revalidation]"
          "[series:value][scalar:floating-point]") {
    const auto switchable = bg::node<switchable_definition>();
    const auto copy = bg::node<copy_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        float_sink(copy(switchable()));
    }

    const bg::CompiledGraph compiled{wiring};
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
    REQUIRE(float_sink_definition::evaluation_count == 1);

    switchable_definition::publish = true;
    runtime.schedule(0);
    runtime.evaluate(time(3));

    CHECK(float_sink_definition::evaluation_count == 2);
    CHECK(float_sink_definition::last_value == 4.0F);
}

namespace {

struct paired_source_definition {
    int index;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        if (valid[index]) {
            output.set(values[index]);
        } else {
            output.invalidate();
        }
    }

    inline static std::array values{1.0F, 2.0F};
    inline static std::array valid{true, true};
};

struct required_pair_definition {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(*lhs + *rhs);
    }

    void on_invalidation() const { ++invalidation_count; }

    inline static int evaluation_count{};
    inline static int invalidation_count{};
};

} // namespace

TEST_CASE("a node resumes after all required inputs become valid",
          "[evaluation][validity][feature:validity][validity:required][transition:revalidation]"
          "[series:value][scalar:floating-point]") {
    const auto paired_source = bg::node<paired_source_definition>();
    const auto required_pair = bg::node<required_pair_definition>();
    paired_source_definition::values = {1.0F, 2.0F};
    paired_source_definition::valid = {true, true};
    required_pair_definition::evaluation_count = 0;
    required_pair_definition::invalidation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        const auto lhs = paired_source(0);
        const auto rhs = paired_source(1);
        (void)required_pair(lhs, rhs);
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));
    REQUIRE(required_pair_definition::evaluation_count == 1);

    paired_source_definition::valid = {false, false};
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(2));

    REQUIRE(required_pair_definition::evaluation_count == 1);
    REQUIRE(required_pair_definition::invalidation_count == 1);

    paired_source_definition::valid[0] = true;
    runtime.schedule(0);
    runtime.evaluate(time(3));
    CHECK(required_pair_definition::evaluation_count == 1);

    paired_source_definition::valid[1] = true;
    runtime.schedule(1);
    runtime.evaluate(time(4));

    CHECK(required_pair_definition::evaluation_count == 2);
    CHECK(required_pair_definition::invalidation_count == 1);
}
