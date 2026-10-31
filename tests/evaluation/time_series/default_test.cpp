#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <vector>

using namespace bg;

TEST_CASE("a default value supplies an initially invalid source input",
          "[feature:operator][feature:validity][series:value][scalar:integer]"
          "[shape:single-node][activity:active][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TS<i64>> input) { return operators::default_(input, 7); };

    const auto actual = bg::run_graph(graph, {literal::SKIP, 3, literal::SKIP, 4});

    CHECK(actual.equals<i64>({7, 3, std::nullopt, 4}));
}

namespace {

struct changing_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.set(20);
        } else if (*step == 1) {
            output.invalidate();
        } else {
            output.set(30);
            ++*step;
            return;
        }

        ++*step;
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct initially_invalid_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(10); }
};

auto time(i64 microseconds) { return MIN_DT + std::chrono::microseconds{microseconds}; }

struct default_observation {
    i64 value;
    EvaluationTime last_updated;
    bool modified;
    EvaluationTime evaluation_time;

    friend bool operator==(const default_observation &, const default_observation &) = default;
};

struct default_observer_definition {
    In<TS<i64>> input;
    Out<TS<default_observation>> output;

    void evaluate(EvaluationContext *context) const {
        output.set(default_observation{*input, input.last_updated(), input.modified(), context->evaluation_time()});
    }
};

const auto observe_default = node<default_observer_definition>();

struct direct_default_observer_definition {
    inline static std::vector<default_observation> observations{};

    In<TS<i64>> input;

    void evaluate(EvaluationContext *context) const {
        observations.push_back({*input, input.last_updated(), input.modified(), context->evaluation_time()});
    }
};

} // namespace

TEST_CASE("a default binding dereferences a reference source",
          "[feature:operator][feature:reference][feature:validity][series:value][series:reference]"
          "[binding:reference-to-value][scalar:integer][transition:invalidation]") {
    const auto source = node<changing_source>();

    const auto pass = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto graph = [&] { return operators::default_(pass(source()), 7); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({10, 20, 7, 30}));
}

TEST_CASE("a default binding switches on source invalidation and revalidation",
          "[feature:operator][feature:validity][feature:invalidation][feature:scheduling][series:value]"
          "[scalar:integer][transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_source>();

    const auto graph = [&] { return operators::default_(source(), 7); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({10, 20, 7, 30}));
}

TEST_CASE("a default binding updates its timestamp across source validity changes",
          "[feature:operator][feature:validity][feature:invalidation][feature:scheduling][series:value]"
          "[scalar:integer][shape:chain][transition:update][transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_source>();

    const auto graph = [&] { return observe_default(operators::default_(source(), 7)); };
    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<default_observation>({default_observation{10, time(1), true, time(1)},
                                              default_observation{20, time(2), true, time(2)},
                                              default_observation{7, time(3), true, time(3)},
                                              default_observation{30, time(4), true, time(4)}}));
}

namespace {

struct equal_default_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(7);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            ++*step;
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.set(7);
    }
};

} // namespace

TEST_CASE("a default binding publishes equal fallback validity switches",
          "[feature:operator][feature:validity][feature:invalidation][feature:scheduling][series:value]"
          "[scalar:integer][shape:chain][transition:invalidation][transition:revalidation]") {
    const auto source = node<equal_default_source>();

    const auto graph = [&] { return observe_default(operators::default_(source(), 7)); };
    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<default_observation>({default_observation{7, time(1), true, time(1)},
                                              default_observation{7, time(2), true, time(2)},
                                              default_observation{7, time(3), true, time(3)}}));
}

TEST_CASE("a reference over a default binding receives fallback updates",
          "[feature:operator][feature:reference][feature:validity][feature:invalidation][series:value]"
          "[series:reference][scalar:integer][shape:chain][binding:value-to-reference]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_source>();

    const auto pass = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto graph = [&] { return observe_default(pass(operators::default_(source(), 7))); };
    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<default_observation>({default_observation{10, time(1), true, time(1)},
                                              default_observation{20, time(2), true, time(2)},
                                              default_observation{7, time(3), true, time(3)},
                                              default_observation{30, time(4), true, time(4)}}));
}

TEST_CASE("a default binding initializes a reference consumer while its source is invalid",
          "[feature:operator][feature:reference][feature:validity][feature:scheduling][series:value]"
          "[series:reference][scalar:integer][shape:chain][binding:value-to-reference]"
          "[binding:reference-to-value][transition:first-tick][transition:revalidation]") {
    const auto source = node<initially_invalid_source>();

    const auto pass = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto graph = [&] { return observe_default(pass(operators::default_(source(), 7))); };
    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<default_observation>({default_observation{7, time(1), true, time(1)},
                                              default_observation{10, time(2), true, time(2)}}));
}

TEST_CASE("nested defaults preserve the first fallback",
          "[feature:operator][feature:validity][feature:scheduling][series:value][scalar:integer]"
          "[shape:chain][transition:first-tick][transition:revalidation]") {
    const auto source = node<initially_invalid_source>();

    const auto observe = node([](i64 input) { return input; });

    const auto graph = [&] {
        return observe(operators::default_(operators::default_(source(), 7), 9));
    };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({7, 10}));
}

TEST_CASE("a nested graph returns a reference over a default binding",
          "[feature:operator][feature:nested-graph][feature:reference][feature:validity][series:value]"
          "[series:reference][scalar:integer][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    using namespace literals;

    const auto source = node<changing_source>();

    const auto inner = graph("name"_n = "default_inner", [](wp<TS<i64>> input) {
        return operators::default_(input, 7).as<TS<i64>>();
    });

    const auto outer = [&] { return nested(inner, operators::const_(true), source()); };

    const auto actual = bg::run_graph(outer);

    CHECK(actual.equals({10, 20, 7, 30}));
}

TEST_CASE("a default binding directly reaches a callable value consumer",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:single-node]"
          "[definition:function]") {
    auto observed = i64{};

    const auto sink = node([&](i64 input) { observed = input; });
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(operators::default_(operators::const_(i64{3}), 7));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).parameters()[0].mode() == ParameterBindingMode::default_value);

    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(observed == 3);
}

TEST_CASE("a default binding has no default node for a value consumer",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:single-node]") {
    direct_default_observer_definition::observations.clear();

    const auto sink = node<direct_default_observer_definition>();

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto adapted = operators::default_(operators::const_(i64{3}), 7);
        CHECK(&adapted.type() == &TS<i64>::type());
        static_cast<void>(sink(adapted));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).parameters()[0].mode() == ParameterBindingMode::default_value);
    CHECK(sizeof(ParameterBinding) == 8);

    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(direct_default_observer_definition::observations ==
          std::vector{default_observation{3, time(1), true, time(1)}});
}

TEST_CASE("a default binding has an ordinary result for a final source",
          "[feature:operator][feature:wiring][series:value][scalar:integer][guarantee:ordinary][guarantee:final]") {
    const auto source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};

    const auto adapted = operators::default_(source(), 7);

    CHECK(&adapted.type() == &TS<i64>::type());
}

TEST_CASE("a default binding materializes only for a reference consumer",
          "[feature:operator][feature:reference][feature:graph-compilation][series:reference][series:value]"
          "[binding:value-to-reference][scalar:integer][shape:chain]") {
    auto evaluations = 0;
    auto valid = false;

    const auto sink = node([&](In<REF<TS<i64>>> input) {
        ++evaluations;
        valid = input.valid();
    });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto source = operators::const_(i64{3});
        sink(operators::default_(source, 7));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 2);

    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(evaluations == 1);
    CHECK(valid);
}

TEST_CASE("equal default adapter nodes deduplicate",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:fan-out]") {
    const auto first_sink = node([](In<TS<i64>>, Out<TS<i64>>) {});

    const auto second_sink = node([](In<TS<i64>>, Out<TS<i64>>) {});

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto source = operators::const_(i64{3});
        static_cast<void>(first_sink(operators::default_(source, 7)));
        static_cast<void>(second_sink(operators::default_(source, 7)));
    }

    wiring.finalize();
    CHECK(wiring.nodes().size() == 5);
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);
}

TEST_CASE("a default binding adds one timestamp to runtime storage",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:single-node]") {
    const auto sink = node([](In<TS<i64>>, Out<TS<i64>>) {});
    const auto runtime_storage_size = [&](bool with_default) {
        auto wiring = GraphWiring{};
        {
            auto context = WiringContext{wiring};
            const auto source = operators::const_(i64{3});
            if (with_default) {
                static_cast<void>(sink(operators::default_(source, 7)));
            } else {
                static_cast<void>(sink(source));
            }
        }
        wiring.finalize();
        return CompiledGraph{wiring}.runtime_storage_size();
    };

    CHECK(runtime_storage_size(true) == runtime_storage_size(false) + sizeof(EvaluationTime));
}

TEST_CASE("graph structure distinguishes default fallbacks",
          "[feature:operator][feature:wiring][series:value][scalar:integer][shape:single-node]") {
    const auto structure = [](i64 fallback) {
        auto wiring = GraphWiring{};
        auto context = WiringContext{wiring};

        const auto source = node(eval = [](Out<TS<i64>>) {}, deduplicate_wiring = false);

        const auto sink = node([](In<TS<i64>>, Out<TS<i64>>) {});
        const auto input = source();
        const auto result = sink(operators::default_(input, fallback));
        const auto boundaries = std::array<WiringPoint, 1>{input};
        (void)wiring.complete_pending_wiring();
        return details::graph_wiring_structure(wiring, boundaries, static_cast<WiringPoint>(result));
    };

    CHECK_FALSE(structure(7) == structure(8));
}
