#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

using namespace bg;

TEST_CASE("a constant value updates only in the first test graph cycle",
          "[feature:constant][feature:test-graph][series:value][scalar:integer][guarantee:const]"
          "[shape:single-node][transition:first-tick]") {
    const auto graph = [](wp<TS<i64>>) {
        return operators::const_(3);
    };

    CHECK(bg::run_graph(graph, {1, 2}).equals<i64>({3, std::nullopt}));
}

TEST_CASE("a constant waits for another required input",
          "[feature:constant][feature:scheduling][feature:validity][transition:revalidation]") {
    auto evaluations = 0;

    const auto sum = node([&](i64 constant, i64 input) {
        ++evaluations;
        return constant + input;
    });

    const auto graph = [&](wp<TS<i64>> input) { return sum(operators::const_(4), input); };
    const auto actual = bg::run_graph(graph, {std::nullopt, 3});

    CHECK(actual.equals({std::nullopt, 7}));
    CHECK(evaluations == 1);
}

namespace {

auto time(i64 microseconds) { return MIN_DT + std::chrono::microseconds{microseconds}; }

struct constant_binding_sink {
    In<TS<i64, TimeSeriesGuarantee::const_>> input;

    void evaluate(EvaluationContext *context) const {
        ++evaluations;
        value = *input;
        last_updated = input.last_updated();
        evaluation_time = context->evaluation_time();
        modified = input.modified();
    }

    inline static int evaluations{};
    inline static i64 value{};
    inline static EvaluationTime last_updated{MIN_DT};
    inline static EvaluationTime evaluation_time{MIN_DT};
    inline static bool modified{};
};

} // namespace

TEST_CASE("a scalar constant binds directly to its active consumer",
          "[feature:constant][feature:graph-compilation][feature:scheduling][series:value][scalar:integer]"
          "[guarantee:const][activity:active][validity:required][transition:first-tick]") {
    constant_binding_sink::evaluations = 0;

    const auto sink = node<constant_binding_sink>();

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(operators::const_(i64{42}));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    REQUIRE(compiled.node(0).parameters()[0].mode() == ParameterBindingMode::constant);
    CHECK(sizeof(ParameterBinding) == 8);
    CHECK(sizeof(Node) == 2 * sizeof(void *));

    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(constant_binding_sink::evaluations == 1);
    CHECK(constant_binding_sink::value == 42);
    CHECK(constant_binding_sink::last_updated == time(1));
    CHECK(constant_binding_sink::evaluation_time == time(1));
    CHECK(constant_binding_sink::modified);

    runtime.evaluate(time(2));
    CHECK(constant_binding_sink::evaluations == 1);

    auto second_global_state = GlobalState{};
    auto second = RuntimeGraph{compiled, second_global_state};
    second.start(time(3));
    second.evaluate(time(3));
    CHECK(constant_binding_sink::evaluations == 2);
    CHECK(constant_binding_sink::last_updated == time(3));
}

TEST_CASE("one constant serves active and passive consumers",
          "[feature:constant][feature:graph-compilation][feature:input-activity][feature:scheduling][series:value]"
          "[scalar:integer][guarantee:const][shape:fan-out][activity:active][activity:passive]") {
    auto active_evaluations = 0;
    auto passive_evaluations = 0;

    const auto active_sink = node([&](In<TS<i64, TimeSeriesGuarantee::const_>>) { ++active_evaluations; });

    const auto passive_sink = node([&](In<TS<i64, TimeSeriesGuarantee::const_>, passive>) {
        ++passive_evaluations;
    });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto value = operators::const_(i64{7});
        active_sink(value);
        passive_sink(value);
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);

    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(active_evaluations == 1);
    CHECK(passive_evaluations == 0);
}

TEST_CASE("a string constant uses compiled scalar storage",
          "[feature:constant][feature:graph-compilation][series:value][scalar:string][storage:variable]"
          "[guarantee:const][shape:single-node][transition:first-tick]") {
    auto value = std::string{};
    auto last_updated = MIN_DT;

    const auto sink = node([&](In<TS<string, TimeSeriesGuarantee::const_>> input) {
        value = input.value();
        last_updated = input.last_updated();
    });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(operators::const_("fixed"));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(value == "fixed");
    CHECK(last_updated == time(1));
}

TEST_CASE("a constant value-to-reference binding keeps the reference node in the compiled graph",
          "[feature:constant][feature:reference][feature:graph-compilation][series:reference][series:value]"
          "[binding:value-to-reference][scalar:integer][guarantee:const][shape:chain][transition:start]") {
    auto evaluations = 0;
    auto valid = false;
    auto value = i64{};

    const auto pass = node([](In<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>> input,
                              Out<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>> output) {
        output.copy(input);
    });

    const auto sink = node([&](In<TS<i64, TimeSeriesGuarantee::const_>> input) {
        ++evaluations;
        valid = input.valid();
        value = valid ? *input : 0;
    });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(pass(operators::const_(i64{9})));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 3);

    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(evaluations == 1);
    CHECK(valid);
    CHECK(value == 9);
}

TEST_CASE("an inner constant uses its own start time",
          "[feature:constant][feature:nested-graph][feature:scheduling][transition:start]") {
    using namespace literals;

    const auto timestamp = node([](In<TS<i64, TimeSeriesGuarantee::const_>> input) {
        return (input.last_updated() - MIN_DT).count();
    });

    const auto inner = graph("name"_n = "constant_timestamp", [&] {
        return timestamp(operators::const_(i64{1}));
    });

    const auto outer = [&](wp<TS<bool>> run) { return nested(inner, run); };

    const auto actual = bg::run_graph(outer, {false, true});

    CHECK(actual.equals({std::nullopt, 2}));
}
