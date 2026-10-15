#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

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
        output.set(*trigger + *sampled);
    }

    inline static int evaluation_count{};
};

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const { last_value = *input; }

    inline static float last_value{};
};

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

TEST_CASE("an active input schedules evaluation and a passive input supplies its latest value",
          "[evaluation][input_activity][feature:input-activity][activity:active][activity:passive]") {
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

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
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
          "[evaluation][input_activity][validity][feature:input-activity][activity:active]"
          "[validity:optional][transition:invalidation][transition:revalidation]") {
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

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
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

TEST_CASE("a passive optional input does not schedule evaluation",
          "[evaluation][input_activity][validity][feature:input-activity][activity:passive]"
          "[validity:optional]") {
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

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
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
        output.set(*input);
    }

    inline static int evaluation_count{};
};

TEST_CASE("a passive required input propagates invalidation without evaluation",
          "[evaluation][input_activity][validity][feature:input-activity][activity:passive]"
          "[validity:required][transition:invalidation]") {
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

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
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

struct dynamically_controlled_input_definition {
    bg::In<bg::TS<float>, bg::dynamic_activity> input;
    bg::In<bg::TS<float>> control;

    void evaluate() const {
        ++evaluation_count;
        if (*control == 0.0F) {
            input.make_passive();
        } else {
            input.make_active();
        }
    }

    inline static int evaluation_count{};
};

TEST_CASE("a dynamic input can stop and resume its own scheduling",
          "[feature:input-activity][series:value][definition:aggregate][activity:active][shape:multiple]") {
    const auto source = bg::node<controlled_source_definition>();
    const auto controlled = bg::node<dynamically_controlled_input_definition>();
    controlled_source_definition::values = {1.0F, 1.0F};
    controlled_source_definition::valid = {true, true};
    dynamically_controlled_input_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        const auto input = source(0);
        const auto control = source(1);
        controlled(input, control);
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    REQUIRE(dynamically_controlled_input_definition::evaluation_count == 1);

    controlled_source_definition::values[0] = 2.0F;
    runtime.schedule(0);
    runtime.evaluate(time(2));
    REQUIRE(dynamically_controlled_input_definition::evaluation_count == 2);

    controlled_source_definition::values[1] = 0.0F;
    runtime.schedule(1);
    runtime.evaluate(time(3));
    REQUIRE(dynamically_controlled_input_definition::evaluation_count == 3);

    controlled_source_definition::values[0] = 3.0F;
    runtime.schedule(0);
    runtime.evaluate(time(4));
    REQUIRE(dynamically_controlled_input_definition::evaluation_count == 3);

    controlled_source_definition::values[1] = 1.0F;
    runtime.schedule(1);
    runtime.evaluate(time(5));
    REQUIRE(dynamically_controlled_input_definition::evaluation_count == 4);

    controlled_source_definition::values[0] = 4.0F;
    runtime.schedule(0);
    runtime.evaluate(time(6));
    CHECK(dynamically_controlled_input_definition::evaluation_count == 5);
}

struct initially_passive_dynamic_input_definition {
    bg::In<bg::TS<float>, bg::passive, bg::dynamic_activity> input;

    void evaluate() const {
        ++evaluation_count;
        input.make_active();
    }

    inline static int evaluation_count{};
};

TEST_CASE("a passive dynamic input starts passive",
          "[feature:input-activity][series:value][definition:aggregate][activity:passive]") {
    const auto source = bg::node<controlled_source_definition>();
    const auto controlled = bg::node<initially_passive_dynamic_input_definition>();
    controlled_source_definition::values = {1.0F, 1.0F};
    controlled_source_definition::valid = {true, true};
    initially_passive_dynamic_input_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        controlled(source(0));
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));
    REQUIRE(initially_passive_dynamic_input_definition::evaluation_count == 0);

    runtime.schedule(1);
    runtime.evaluate(time(2));
    REQUIRE(initially_passive_dynamic_input_definition::evaluation_count == 1);

    controlled_source_definition::values[0] = 2.0F;
    runtime.schedule(0);
    runtime.evaluate(time(3));
    CHECK(initially_passive_dynamic_input_definition::evaluation_count == 2);
}

TEST_CASE("a callable dynamic input controls only its own scheduling",
          "[feature:input-activity][series:value][definition:function][activity:active][shape:multiple]") {
    const auto gate = bg::node([](bg::In<bg::TS<bg::i64>, bg::dynamic_activity> input,
                                  bg::In<bg::TS<bg::i64>, bg::optional> control, bg::Out<bg::TS<bg::i64>> output) {
        if (control.valid()) {
            if (*control == 0) {
                input.make_passive();
            } else {
                input.make_active();
            }
        }
        output.set(*input);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input, bg::wp<bg::TS<bg::i64>> control) {
        return gate(input, control);
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<bg::i64, 6>{1, 2, 3, 4, 5, 6},
        std::array<std::optional<bg::i64>, 6>{1, std::nullopt, 0, std::nullopt, 1, std::nullopt});

    CHECK(actual.equals<bg::i64>({1, 2, 3, std::nullopt, 5, 6}));
}

TEST_CASE("a dynamic input controls scheduling for a default binding",
          "[feature:input-activity][feature:operator][series:value][definition:function]"
          "[activity:active][shape:multiple]") {
    const auto gate = bg::node([](bg::In<bg::TS<bg::i64>, bg::dynamic_activity> input,
                                  bg::In<bg::TS<bg::i64>, bg::optional> control, bg::Out<bg::TS<bg::i64>> output) {
        if (control.valid()) {
            if (*control == 0) {
                input.make_passive();
            } else {
                input.make_active();
            }
        }
        output.set(*input);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input, bg::wp<bg::TS<bg::i64>> control) {
        return gate(bg::operators::default_(input, 7), control);
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<std::optional<bg::i64>, 6>{std::nullopt, 1, 2, 3, 4, 5},
        std::array<std::optional<bg::i64>, 6>{1, std::nullopt, 0, std::nullopt, 1, std::nullopt});

    CHECK(actual.equals<bg::i64>({7, 1, 2, std::nullopt, 4, 5}));
}

struct independently_controlled_inputs_definition {
    bg::In<bg::TS<float>, bg::dynamic_activity> left;
    bg::In<bg::TS<float>, bg::dynamic_activity> right;
    bg::In<bg::TS<float>> control;

    void evaluate() const {
        ++evaluation_count;

        if (*control == 0.0F) {
            left.make_passive();
            right.make_active();
        } else if (*control == 1.0F) {
            left.make_active();
            right.make_passive();
        } else {
            left.make_passive();
            right.make_passive();
        }
    }

    inline static int evaluation_count{};
};

TEST_CASE("two dynamic inputs sharing one time series control scheduling independently",
          "[feature:input-activity][series:value][definition:aggregate][activity:active][shape:fan-out]") {
    const auto source = bg::node<controlled_source_definition>();
    const auto controlled = bg::node<independently_controlled_inputs_definition>();
    controlled_source_definition::values = {1.0F, 0.0F};
    controlled_source_definition::valid = {true, true};
    independently_controlled_inputs_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        const auto value = source(0);
        const auto control = source(1);
        controlled(value, value, control);
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));
    REQUIRE(independently_controlled_inputs_definition::evaluation_count == 1);

    runtime.schedule(0);
    runtime.evaluate(time(2));
    REQUIRE(independently_controlled_inputs_definition::evaluation_count == 2);

    controlled_source_definition::values[1] = 2.0F;
    runtime.schedule(1);
    runtime.evaluate(time(3));
    REQUIRE(independently_controlled_inputs_definition::evaluation_count == 3);

    runtime.schedule(0);
    runtime.evaluate(time(4));
    CHECK(independently_controlled_inputs_definition::evaluation_count == 3);

    controlled_source_definition::values[1] = 1.0F;
    runtime.schedule(1);
    runtime.evaluate(time(5));
    REQUIRE(independently_controlled_inputs_definition::evaluation_count == 4);

    runtime.schedule(0);
    runtime.evaluate(time(6));
    CHECK(independently_controlled_inputs_definition::evaluation_count == 5);
}

struct dynamic_required_copy_definition {
    bg::In<bg::TS<float>, bg::dynamic_activity> input;
    bg::In<bg::TS<float>> control;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        if (*control == 0.0F) {
            input.make_passive();
        } else {
            input.make_active();
        }
        output.set(*input);
    }

    inline static int evaluation_count{};
};

TEST_CASE("a dynamically passive required input still propagates invalidation",
          "[feature:input-activity][feature:validity][series:value][definition:aggregate]"
          "[activity:passive][validity:required][transition:invalidation]") {
    const auto source = bg::node<controlled_source_definition>();
    const auto copy = bg::node<dynamic_required_copy_definition>();
    const auto observer = bg::node<active_optional_observer_definition>();
    controlled_source_definition::values = {1.0F, 0.0F};
    controlled_source_definition::valid = {true, true};
    dynamic_required_copy_definition::evaluation_count = 0;
    active_optional_observer_definition::observation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        const auto input = source(0);
        const auto control = source(1);
        observer(copy(input, control));
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));
    REQUIRE(dynamic_required_copy_definition::evaluation_count == 1);
    REQUIRE(active_optional_observer_definition::observation_count == 1);
    REQUIRE(active_optional_observer_definition::observed[0]);

    controlled_source_definition::valid[0] = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    CHECK(dynamic_required_copy_definition::evaluation_count == 1);
    REQUIRE(active_optional_observer_definition::observation_count == 2);
    CHECK_FALSE(active_optional_observer_definition::observed[1]);
}

struct dynamic_optional_observer_definition {
    bg::In<bg::TS<float>, bg::optional, bg::dynamic_activity> input;
    bg::In<bg::TS<float>> control;

    void evaluate() const {
        if (*control == 0.0F) {
            input.make_passive();
        } else {
            input.make_active();
        }
        observed[observation_count++] = input.valid();
    }

    inline static std::array<bool, 5> observed{};
    inline static std::size_t observation_count{};
};

TEST_CASE("a dynamic optional input applies its current activity to validity changes",
          "[feature:input-activity][feature:validity][series:value][definition:aggregate]"
          "[activity:active][activity:passive][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = bg::node<controlled_source_definition>();
    const auto observer = bg::node<dynamic_optional_observer_definition>();
    controlled_source_definition::values = {1.0F, 1.0F};
    controlled_source_definition::valid = {true, true};
    dynamic_optional_observer_definition::observation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        const auto input = source(0);
        const auto control = source(1);
        observer(input, control);
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(1));

    controlled_source_definition::valid[0] = false;
    runtime.schedule(0);
    runtime.evaluate(time(2));

    controlled_source_definition::valid[0] = true;
    controlled_source_definition::values[1] = 0.0F;
    runtime.schedule(0);
    runtime.schedule(1);
    runtime.evaluate(time(3));

    controlled_source_definition::valid[0] = false;
    runtime.schedule(0);
    runtime.evaluate(time(4));
    REQUIRE(dynamic_optional_observer_definition::observation_count == 3);

    controlled_source_definition::values[1] = 1.0F;
    runtime.schedule(1);
    runtime.evaluate(time(5));

    controlled_source_definition::valid[0] = true;
    runtime.schedule(0);
    runtime.evaluate(time(6));

    REQUIRE(dynamic_optional_observer_definition::observation_count == 5);
    CHECK(dynamic_optional_observer_definition::observed == std::array{true, false, true, false, true});
}

TEST_CASE("a dynamic input connected to a collection item controls its scheduling",
          "[feature:input-activity][feature:collection-change][series:fixed-tsl][definition:function]"
          "[activity:active][shape:chain]") {
    const auto gate =
        bg::node([](bg::In<bg::TS<bg::i64>, bg::dynamic_activity> input, bg::Out<bg::TS<bg::i64>> output) {
            output.set(*input);
            input.make_passive();
        });
    const auto graph = [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 1>> input) { return gate(input[0]); };

    CHECK(bg::testing::test_graph(graph, {{{0, 1}}, {{0, 2}}})
              .equals<bg::i64>({1, std::nullopt}));
}

struct runtime_local_activity_definition {
    bg::In<bg::TS<float>, bg::dynamic_activity> input;

    void evaluate() const {
        ++evaluation_count;
        if (activate) {
            input.make_active();
        } else {
            input.make_passive();
        }
    }

    inline static bool activate{};
    inline static int evaluation_count{};
};

TEST_CASE("runtime graphs created from one compiled graph have independent input activity",
          "[feature:input-activity][feature:graph-compilation][series:value][definition:aggregate]"
          "[activity:active][shape:multiple]") {
    const auto source = bg::node<controlled_source_definition>();
    const auto controlled = bg::node<runtime_local_activity_definition>();
    controlled_source_definition::values[0] = 1.0F;
    controlled_source_definition::valid[0] = true;
    runtime_local_activity_definition::activate = false;
    runtime_local_activity_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        controlled(source(0));
    }

    wiring.finalize();
    const bg::CompiledGraph compiled{wiring};
    auto first_global_state = bg::GlobalState{};
    bg::RuntimeGraph first{compiled, first_global_state};
    auto second_global_state = bg::GlobalState{};
    bg::RuntimeGraph second{compiled, second_global_state};
    first.start(time(1));
    second.start(time(1));

    first.schedule(0);
    second.schedule(0);
    first.evaluate(time(1));
    second.evaluate(time(1));
    REQUIRE(runtime_local_activity_definition::evaluation_count == 2);

    runtime_local_activity_definition::activate = true;
    first.schedule(1);
    first.evaluate(time(2));
    REQUIRE(runtime_local_activity_definition::evaluation_count == 3);

    first.schedule(0);
    second.schedule(0);
    first.evaluate(time(3));
    second.evaluate(time(2));
    CHECK(runtime_local_activity_definition::evaluation_count == 4);
}

} // namespace
