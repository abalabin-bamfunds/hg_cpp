#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace {

struct controlled_source_definition {
    int index;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        if (valid[index]) {
            output.set(values[index]);
        } else {
            output.invalidate();
        }
    }

    inline static std::array values{1.0F, 10.0F};
    inline static std::array valid{true, true};
};

struct mixed_input_definition {
    bg::In<bg::TS<float>> trigger;
    bg::In<bg::TS<float>, bg::passive> sampled;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(trigger.value() + sampled.value());
    }

    inline static int evaluation_count{};
};

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const { last_value = input.value(); }

    inline static float last_value{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

TEST_CASE("an active input schedules evaluation and a passive input supplies its latest value",
          "[evaluation][input_activity]") {
    const auto controlled_source = bg::node<controlled_source_definition>();
    const auto mixed_input = bg::node<mixed_input_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    controlled_source_definition::values = {1.0F, 10.0F};
    controlled_source_definition::valid = {true, true};
    mixed_input_definition::evaluation_count = 0;
    float_sink_definition::last_value = 0.0F;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        const auto trigger = controlled_source(0);
        const auto sampled = controlled_source(1);
        float_sink(mixed_input(trigger, sampled));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    REQUIRE(mixed_input_definition::evaluation_count == 1);
    REQUIRE(float_sink_definition::last_value == 11.0F);

    controlled_source_definition::values[1] = 20.0F;
    runtime.schedule(1);
    runtime.evaluate(time(2));
    CHECK(mixed_input_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 11.0F);

    controlled_source_definition::values[0] = 2.0F;
    runtime.schedule(0);
    runtime.evaluate(time(3));

    CHECK(mixed_input_definition::evaluation_count == 2);
    CHECK(float_sink_definition::last_value == 22.0F);
}

struct active_optional_observer_definition {
    bg::In<bg::TS<float>, bg::optional> input;

    void evaluate() const { observed[observation_count++] = input.valid(); }

    inline static std::array<bool, 3> observed{};
    inline static std::size_t observation_count{};
};

TEST_CASE("an active optional input schedules evaluation on invalidation and revalidation",
          "[evaluation][input_activity][validity]") {
    const auto controlled_source = bg::node<controlled_source_definition>();
    const auto active_optional_observer = bg::node<active_optional_observer_definition>();
    controlled_source_definition::values[0] = 1.0F;
    controlled_source_definition::valid[0] = true;
    active_optional_observer_definition::observation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        active_optional_observer(controlled_source(0));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    controlled_source_definition::valid[0] = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    controlled_source_definition::valid[0] = true;
    runtime.schedule(0);
    runtime.evaluate(time(3));

    REQUIRE(active_optional_observer_definition::observation_count == 3);
    CHECK(active_optional_observer_definition::observed == std::array{true, false, true});
}

struct passive_optional_observer_definition {
    bg::In<bg::TS<float>, bg::passive, bg::optional> input;

    void evaluate() const {
        ++evaluation_count;
        last_valid = input.valid();
    }

    inline static int evaluation_count{};
    inline static bool last_valid{};
};

TEST_CASE("a passive optional input does not schedule evaluation", "[evaluation][input_activity][validity]") {
    const auto controlled_source = bg::node<controlled_source_definition>();
    const auto passive_optional_observer = bg::node<passive_optional_observer_definition>();
    controlled_source_definition::values[0] = 1.0F;
    controlled_source_definition::valid[0] = true;
    passive_optional_observer_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        passive_optional_observer(controlled_source(0));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    REQUIRE(passive_optional_observer_definition::evaluation_count == 1);
    REQUIRE(passive_optional_observer_definition::last_valid);

    controlled_source_definition::valid[0] = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    controlled_source_definition::valid[0] = true;
    runtime.schedule(0);
    runtime.evaluate(time(3));

    CHECK(passive_optional_observer_definition::evaluation_count == 1);
}

struct passive_copy_definition {
    bg::In<bg::TS<float>, bg::passive> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(input.value());
    }

    inline static int evaluation_count{};
};

TEST_CASE("a passive required input propagates invalidation without evaluation",
          "[evaluation][input_activity][validity]") {
    const auto controlled_source = bg::node<controlled_source_definition>();
    const auto passive_copy = bg::node<passive_copy_definition>();
    const auto active_optional_observer = bg::node<active_optional_observer_definition>();
    controlled_source_definition::values[0] = 1.0F;
    controlled_source_definition::valid[0] = true;
    passive_copy_definition::evaluation_count = 0;
    active_optional_observer_definition::observation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        active_optional_observer(passive_copy(controlled_source(0)));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    REQUIRE(passive_copy_definition::evaluation_count == 1);
    REQUIRE(active_optional_observer_definition::observation_count == 1);
    REQUIRE(active_optional_observer_definition::observed[0]);

    controlled_source_definition::valid[0] = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    CHECK(passive_copy_definition::evaluation_count == 1);
    REQUIRE(active_optional_observer_definition::observation_count == 2);
    CHECK_FALSE(active_optional_observer_definition::observed[1]);

    controlled_source_definition::valid[0] = true;
    runtime.schedule(0);
    runtime.evaluate(time(3));
    CHECK(passive_copy_definition::evaluation_count == 1);

    runtime.schedule(1);
    runtime.evaluate(time(4));

    CHECK(passive_copy_definition::evaluation_count == 2);
    REQUIRE(active_optional_observer_definition::observation_count == 3);
    CHECK(active_optional_observer_definition::observed[2]);
}

} // namespace
