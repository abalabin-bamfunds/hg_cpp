#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

using namespace bg;
using namespace bg::literals;

TEST_CASE("an active input schedules evaluation and a passive input supplies its latest value",
          "[evaluation][input_activity][feature:input-activity][activity:active][activity:passive]") {

    const auto combine = node([](float trigger, In<TS<float>, passive> sampled) { return trigger + *sampled; });

    const auto consume = node([](float sum) { return sum; });

    const auto graph = bg::graph(
        "name"_n = "combine_and_consume", [&](wp<TS<float>> trigger, wp<TS<float>> sampled) {
        return consume(combine(trigger, sampled));
    });

    const auto actual = testing::test_graph(graph, {1.0F, literal::SKIP, 2.0F}, {10.0F, 20.0F, literal::SKIP});

    CHECK(actual.equals({11.0F, literal::SKIP, 22.0F}));
}

TEST_CASE("an active optional input schedules evaluation on invalidation and revalidation",
          "[evaluation][input_activity][validity][feature:input-activity][activity:active]"
          "[validity:optional][transition:invalidation][transition:revalidation]") {

    const auto observe = node([](In<TS<float>, optional> input) { return input.valid(); });

    CHECK(testing::test_graph(observe, {1.0F, literal::INVALID, 1.0F}).equals({true, false, true}));
}

TEST_CASE("a passive optional input does not schedule evaluation",
          "[evaluation][input_activity][validity][feature:input-activity][activity:passive]"
          "[validity:optional]") {

    const auto observe = node([](In<TS<float>, passive, optional> input, bool) { return input.valid(); });

    const auto actual = testing::test_graph(
        observe, {1.0F, literal::INVALID, literal::SKIP, 3.0F, literal::SKIP},
        {true, literal::SKIP, true, literal::SKIP, true});

    CHECK(actual.equals({true, literal::SKIP, false, literal::SKIP, true}));
}

TEST_CASE("a passive required input propagates invalidation without evaluation",
          "[evaluation][input_activity][validity][feature:input-activity][activity:passive]"
          "[validity:required][transition:invalidation]") {

    const auto copy = node([](In<TS<float>, passive> input, bool) { return *input; });

    const auto observe = node([](std::optional<float> input) { return input.value_or(-1.0F); });

    const auto graph = bg::graph("name"_n = "observe_copy", [&](wp<TS<float>> input, wp<TS<bool>> pulse) {
        return observe(copy(input, pulse));
    });

    const auto actual = testing::test_graph(
        graph, {1.0F, literal::INVALID, 3.0F, literal::SKIP}, {true, literal::SKIP, literal::SKIP, true});

    CHECK(actual.equals({1.0F, -1.0F, literal::SKIP, 3.0F}));
}

TEST_CASE("a dynamic input can stop and resume its own scheduling",
          "[feature:input-activity][series:value][definition:aggregate][activity:active][shape:multiple]") {
    struct dynamically_controlled_input {
        In<TS<float>, dynamic_activity> input;
        In<TS<float>> control;
        State<i64> evaluations;
        Out<TS<i64>> output;

        void evaluate() const {
            ++*evaluations;
            if (*control == 0.0F) {
                input.make_passive();
            } else {
                input.make_active();
            }

            output.set(*evaluations);
        }
    };

    const auto controlled = node<dynamically_controlled_input>();
    const auto actual = testing::test_graph(
        controlled, {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F},
        {1.0F, literal::SKIP, 0.0F, literal::SKIP, 1.0F, literal::SKIP});

    CHECK(actual.equals({1, 2, 3, literal::SKIP, 4, 5}));
}

TEST_CASE("a passive dynamic input starts passive",
          "[feature:input-activity][series:value][definition:aggregate][activity:passive]") {
    struct initially_passive_dynamic_input {
        In<TS<float>, passive, dynamic_activity> input;
        In<TS<bool>, optional> activate;
        State<i64> evaluations;
        Out<TS<i64>> output;

        void evaluate() const {
            ++*evaluations;
            if (activate.valid()) {
                input.make_active();
            }

            output.set(*evaluations);
        }
    };

    const auto activate = node<initially_passive_dynamic_input>();
    const auto actual = testing::test_graph(activate, {1.0F, literal::SKIP, 2.0F},
                                            {literal::SKIP, true, literal::SKIP});

    CHECK(actual.equals({literal::SKIP, 1, 2}));
}

TEST_CASE("a callable dynamic input controls only its own scheduling",
          "[feature:input-activity][series:value][definition:function][activity:active][shape:multiple]") {

    const auto gate = node([](In<TS<i64>, dynamic_activity> input, In<TS<i64>, optional> control) {
        if (control.valid()) {
            if (*control == 0) {
                input.make_passive();
            } else {
                input.make_active();
            }
        }
        return *input;
    });

    const auto actual = testing::test_graph(gate, {1, 2, 3, 4, 5, 6},
                                            {1, literal::SKIP, 0, literal::SKIP, 1, literal::SKIP});

    CHECK(actual.equals({1, 2, 3, literal::SKIP, 5, 6}));
}

TEST_CASE("a dynamic input controls scheduling for a default binding",
          "[feature:input-activity][feature:operator][series:value][definition:function]"
          "[activity:active][shape:multiple]") {

    const auto gate = node([](In<TS<i64>, dynamic_activity> input, In<TS<i64>, optional> control) {
        if (control.valid()) {
            if (*control == 0) {
                input.make_passive();
            } else {
                input.make_active();
            }
        }
        return *input;
    });

    const auto graph = bg::graph(
        "name"_n = "default_binding_activity", [&](wp<TS<i64>> input, wp<TS<i64>> control) {
        return gate(operators::default_(input, 7), control);
    });

    const auto actual = testing::test_graph(
        graph, {literal::INVALID, 1, 2, 3, 4, 5}, {1, literal::SKIP, 0, literal::SKIP, 1, literal::SKIP});

    CHECK(actual.equals({7, 1, 2, literal::SKIP, 4, 5}));
}

TEST_CASE("two dynamic inputs sharing one time series control scheduling independently",
          "[feature:input-activity][series:value][definition:aggregate][activity:active][shape:fan-out]") {
    struct independently_controlled_inputs {
        In<TS<float>, dynamic_activity> left;
        In<TS<float>, dynamic_activity> right;
        In<TS<float>> control;
        State<i64> evaluations;
        Out<TS<i64>> output;

        void evaluate() const {
            ++*evaluations;

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

            output.set(*evaluations);
        }
    };

    const auto control_activity = node<independently_controlled_inputs>();

    const auto graph = bg::graph(
        "name"_n = "independent_shared_input_activity", [&](wp<TS<float>> value, wp<TS<float>> control) {
        return control_activity(value, value, control);
    });

    const auto actual = testing::test_graph(
        graph, {1.0F, 2.0F, literal::SKIP, 3.0F, literal::SKIP, 4.0F},
        {0.0F, literal::SKIP, 2.0F, literal::SKIP, 1.0F, literal::SKIP});

    CHECK(actual.equals({1, 2, 3, literal::SKIP, 4, 5}));
}

TEST_CASE("a dynamically passive required input still propagates invalidation",
          "[feature:input-activity][feature:validity][series:value][definition:aggregate]"
          "[activity:passive][validity:required][transition:invalidation]") {
    struct dynamic_required_copy {
        In<TS<float>, dynamic_activity> input;
        In<TS<float>> control;
        State<i64> evaluations;
        Out<TS<i64>> output;

        void evaluate() const {
            ++*evaluations;
            if (*control == 0.0F) {
                input.make_passive();
            } else {
                input.make_active();
            }
            output.set(*evaluations);
        }
    };

    const auto copy = node<dynamic_required_copy>();
    const auto actual = testing::test_graph(copy, {1.0F, literal::INVALID, 3.0F}, {0.0F, literal::SKIP, 1.0F});

    CHECK(actual.equals({1, literal::INVALID, 2}));
}

TEST_CASE("a dynamic optional input applies its current activity to validity changes",
          "[feature:input-activity][feature:validity][series:value][definition:aggregate]"
          "[activity:active][activity:passive][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    struct dynamic_optional_observer {
        In<TS<float>, optional, dynamic_activity> input;
        In<TS<float>> control;
        Out<TS<bool>> output;

        void evaluate() const {
            if (*control == 0.0F) {
                input.make_passive();
            } else {
                input.make_active();
            }

            output.set(input.valid());
        }
    };

    const auto observe = node<dynamic_optional_observer>();

    const auto actual = testing::test_graph(
        observe, {1.0F, literal::INVALID, 1.0F, literal::INVALID, literal::SKIP, 1.0F},
        {1.0F, literal::SKIP, 0.0F, literal::SKIP, 1.0F, literal::SKIP});

    CHECK(actual.equals({true, false, true, literal::SKIP, false, true}));
}

TEST_CASE("a dynamic input connected to a collection item controls its scheduling",
          "[feature:input-activity][feature:collection-change][series:fixed-tsl][definition:function]"
          "[activity:active][shape:chain]") {

    const auto gate = node([](In<TS<i64>, dynamic_activity> input) {
        const auto value = *input;
        input.make_passive();
        return value;
    });

    const auto graph = [&](wp<TSL<TS<i64>, 1>> input) { return gate(input[0]); };

    CHECK(testing::test_graph(graph, {{{0, 1}}, {{0, 2}}}).equals<i64>({1, std::nullopt}));
}

namespace {

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

struct runtime_local_activity_definition {
    In<TS<float>, dynamic_activity> input;

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

} // namespace

TEST_CASE("runtime graphs created from one compiled graph have independent input activity",
          "[feature:input-activity][feature:graph-compilation][series:value][definition:aggregate]"
          "[activity:active][shape:multiple]") {

    const auto source = node([] { return 1.0F; });

    const auto controlled = node<runtime_local_activity_definition>();
    runtime_local_activity_definition::activate = false;
    runtime_local_activity_definition::evaluation_count = 0;

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        controlled(source());
    }

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto first_global_state = GlobalState{};
    RuntimeGraph first{compiled, first_global_state};
    auto second_global_state = GlobalState{};
    RuntimeGraph second{compiled, second_global_state};
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
    CHECK(runtime_local_activity_definition::evaluation_count == 4);
    second.evaluate(time(2));
    CHECK(runtime_local_activity_definition::evaluation_count == 4);
}
