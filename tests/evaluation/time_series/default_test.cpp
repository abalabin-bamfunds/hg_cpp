#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace {

struct default_sink {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluations;
        value = *input;
        last_updated = input.last_updated();
        modified = input.modified();
        evaluation_time = context->evaluation_time();
    }

    inline static int evaluations{};
    inline static bg::i64 value{};
    inline static bg::EvaluationTime last_updated{bg::MIN_DT};
    inline static bg::EvaluationTime evaluation_time{bg::MIN_DT};
    inline static bool modified{};
};

struct default_reference_sink {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;

    void evaluate() const {
        ++evaluations;
        valid = input.valid();
    }

    inline static int evaluations{};
    inline static bool valid{};
};

struct default_timeline_sink {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate(bg::EvaluationContext *context) const {
        values.push_back(*input);
        last_updated.push_back(input.last_updated());
        modified.push_back(input.modified());
        evaluation_times.push_back(context->evaluation_time());
    }

    inline static std::vector<bg::i64> values;
    inline static std::vector<bg::EvaluationTime> last_updated;
    inline static std::vector<bool> modified;
    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

struct reference_pass {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;

    void evaluate() const { output.copy(input); }
};

struct changing_source {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
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

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(10); }
};

struct equal_default_source {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(7);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            ++*step;
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.set(7);
    }
};

struct default_structure_source {
    static constexpr bool deduplicate_wiring = false;

    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {}
};

struct default_structure_sink {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {}
};

struct second_default_structure_sink {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {}
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a default value supplies an initially invalid source input",
          "[feature:operator][feature:validity][series:value][scalar:integer]"
          "[shape:single-node][activity:active][validity:required][transition:first-tick]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) {
        return bg::operators::default_(input, 7).as<bg::TS<bg::i64>>();
    };

    const auto actual =
        bg::testing::test_graph(graph, std::array<std::optional<bg::i64>, 4>{std::nullopt, 3, std::nullopt, 4});

    CHECK(actual.equals({7, 3, std::nullopt, 4}));
}

TEST_CASE("a default binding has no default node for a value consumer",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:single-node]") {
    using namespace bg::operators;

    default_sink::evaluations = 0;
    auto wiring = bg::GraphWiring{};
    const auto sink = bg::node<default_sink>();
    {
        const auto context = bg::WiringContext{wiring};
        const auto source = bg::operators::const_(3).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        const auto adapted = default_(source, 7);
        CHECK(&adapted.type() == &bg::TS<bg::i64>::type());
        sink(adapted);
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).parameters()[0].mode() == bg::ParameterBindingMode::default_value);
    CHECK(sizeof(bg::ParameterBinding) == 8);

    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(default_sink::evaluations == 1);
    CHECK(default_sink::value == 3);
    CHECK(default_sink::last_updated == time(1));
    CHECK(default_sink::evaluation_time == time(1));
    CHECK(default_sink::modified);
}

TEST_CASE("a default binding has an ordinary result for a final source",
          "[feature:operator][feature:wiring][series:value][scalar:integer][guarantee:ordinary][guarantee:final]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto adapted = bg::operators::default_(source(), 7);

    CHECK(&adapted.type() == &bg::TS<bg::i64>::type());
}

TEST_CASE("a default binding directly reaches a callable value consumer",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:single-node]"
          "[definition:function]") {
    auto observed = bg::i64{};
    const auto sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { observed = *input; });
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        static_cast<void>(sink(bg::operators::default_(bg::operators::const_(3), 7)));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    CHECK(compiled.node(0).parameters()[0].mode() == bg::ParameterBindingMode::default_value);

    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(observed == 3);
}

TEST_CASE("a default binding materializes only for a reference consumer",
          "[feature:operator][feature:reference][feature:graph-compilation][series:reference][series:value]"
          "[binding:value-to-reference][scalar:integer][shape:chain]") {
    using namespace bg::operators;

    default_reference_sink::evaluations = 0;
    default_reference_sink::valid = false;
    auto wiring = bg::GraphWiring{};
    const auto sink = bg::node<default_reference_sink>();
    {
        const auto context = bg::WiringContext{wiring};
        const auto source = const_(3).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        sink(default_(source, 7));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 2);

    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(default_reference_sink::evaluations == 1);
    CHECK(default_reference_sink::valid);
}

TEST_CASE("a default binding dereferences a reference source",
          "[feature:operator][feature:reference][feature:validity][series:value][series:reference]"
          "[binding:reference-to-value][scalar:integer][transition:invalidation]") {
    const auto source = bg::node<changing_source>();
    const auto pass = bg::node<reference_pass>();
    const auto graph = [&] { return bg::operators::default_(pass(source()), 7).as<bg::TS<bg::i64>>(); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20, 7, 30}));
}

TEST_CASE("a default binding switches on source invalidation and revalidation",
          "[feature:operator][feature:validity][feature:invalidation][feature:scheduling][series:value]"
          "[scalar:integer][transition:invalidation][transition:revalidation]") {
    const auto source = bg::node<changing_source>();
    const auto graph = [&] { return bg::operators::default_(source(), 7).as<bg::TS<bg::i64>>(); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20, 7, 30}));
}

TEST_CASE("a default binding updates its timestamp across source validity changes",
          "[feature:operator][feature:validity][feature:invalidation][feature:scheduling][series:value]"
          "[scalar:integer][shape:chain][transition:update][transition:invalidation][transition:revalidation]") {
    const auto source = bg::node<changing_source>();
    const auto sink = bg::node<default_timeline_sink>();
    default_timeline_sink::values.clear();
    default_timeline_sink::last_updated.clear();
    default_timeline_sink::modified.clear();
    default_timeline_sink::evaluation_times.clear();

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        sink(bg::operators::default_(source(), 7));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(default_timeline_sink::values == std::vector<bg::i64>{10, 20, 7, 30});
    CHECK(default_timeline_sink::last_updated == std::vector{time(1), time(2), time(3), time(4)});
    CHECK(default_timeline_sink::modified == std::vector<bool>{true, true, true, true});
    CHECK(default_timeline_sink::evaluation_times == default_timeline_sink::last_updated);
}

TEST_CASE("a default binding publishes equal fallback validity switches",
          "[feature:operator][feature:validity][feature:invalidation][feature:scheduling][series:value]"
          "[scalar:integer][shape:chain][transition:invalidation][transition:revalidation]") {
    const auto source = bg::node<equal_default_source>();
    const auto sink = bg::node<default_timeline_sink>();
    default_timeline_sink::values.clear();
    default_timeline_sink::last_updated.clear();
    default_timeline_sink::modified.clear();
    default_timeline_sink::evaluation_times.clear();

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        sink(bg::operators::default_(source(), 7));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(default_timeline_sink::values == std::vector<bg::i64>{7, 7, 7});
    CHECK(default_timeline_sink::last_updated == std::vector{time(1), time(2), time(3)});
    CHECK(default_timeline_sink::modified == std::vector<bool>{true, true, true});
}

TEST_CASE("a reference over a default binding receives fallback updates",
          "[feature:operator][feature:reference][feature:validity][feature:invalidation][series:value]"
          "[series:reference][scalar:integer][shape:chain][binding:value-to-reference]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    const auto source = bg::node<changing_source>();
    const auto pass = bg::node<reference_pass>();
    const auto sink = bg::node<default_timeline_sink>();
    default_timeline_sink::values.clear();
    default_timeline_sink::last_updated.clear();
    default_timeline_sink::modified.clear();
    default_timeline_sink::evaluation_times.clear();

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        sink(pass(bg::operators::default_(source(), 7)));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(default_timeline_sink::values == std::vector<bg::i64>{10, 20, 7, 30});
    CHECK(default_timeline_sink::last_updated == std::vector{time(1), time(2), time(3), time(4)});
}

TEST_CASE("a default binding initializes a reference consumer while its source is invalid",
          "[feature:operator][feature:reference][feature:validity][feature:scheduling][series:value]"
          "[series:reference][scalar:integer][shape:chain][binding:value-to-reference]"
          "[binding:reference-to-value][transition:first-tick][transition:revalidation]") {
    const auto source = bg::node<initially_invalid_source>();
    const auto pass = bg::node<reference_pass>();
    const auto sink = bg::node<default_timeline_sink>();
    default_timeline_sink::values.clear();
    default_timeline_sink::last_updated.clear();
    default_timeline_sink::modified.clear();
    default_timeline_sink::evaluation_times.clear();

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        sink(pass(bg::operators::default_(source(), 7)));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(default_timeline_sink::values == std::vector<bg::i64>{7, 10});
    CHECK(default_timeline_sink::last_updated == std::vector{time(1), time(2)});
}

TEST_CASE("nested defaults preserve the first fallback",
          "[feature:operator][feature:validity][feature:scheduling][series:value][scalar:integer]"
          "[shape:chain][transition:first-tick][transition:revalidation]") {
    const auto source = bg::node<initially_invalid_source>();
    const auto sink = bg::node<default_timeline_sink>();
    default_timeline_sink::values.clear();
    default_timeline_sink::last_updated.clear();
    default_timeline_sink::modified.clear();
    default_timeline_sink::evaluation_times.clear();

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        sink(bg::operators::default_(bg::operators::default_(source(), 7), 9));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(default_timeline_sink::values == std::vector<bg::i64>{7, 10});
}

TEST_CASE("a nested graph returns a reference over a default binding",
          "[feature:operator][feature:nested-graph][feature:reference][feature:validity][series:value]"
          "[series:reference][scalar:integer][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    using namespace bg::literals;

    const auto source = bg::node<changing_source>();
    const auto inner =
        bg::graph("name"_n = "default_inner", [](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
            return bg::operators::default_(input, 7).as<bg::TS<bg::i64>>();
        });
    const auto graph = [&] { return bg::nested(inner, bg::operators::const_(true), source()).as<bg::TS<bg::i64>>(); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20, 7, 30}));
}

TEST_CASE("equal default adapter nodes deduplicate",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:fan-out]") {
    const auto first_sink = bg::node<default_structure_sink>();
    const auto second_sink = bg::node<second_default_structure_sink>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        const auto source = bg::operators::const_(3);
        static_cast<void>(first_sink(bg::operators::default_(source, 7)));
        static_cast<void>(second_sink(bg::operators::default_(source, 7)));
    }

    wiring.finalize();
    CHECK(wiring.nodes().size() == 5);
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);
}

TEST_CASE("a default binding adds one timestamp to runtime storage",
          "[feature:operator][feature:graph-compilation][series:value][scalar:integer][shape:single-node]") {
    const auto sink = bg::node<default_structure_sink>();
    const auto runtime_storage_size = [&](bool with_default) {
        auto wiring = bg::GraphWiring{};
        {
            const auto context = bg::WiringContext{wiring};
            const auto source = bg::operators::const_(3).as<bg::TS<bg::i64>>();
            static_cast<void>(with_default ? sink(bg::operators::default_(source, 7)) : sink(source));
        }
        wiring.finalize();
        return bg::CompiledGraph{wiring}.runtime_storage_size();
    };

    CHECK(runtime_storage_size(true) == runtime_storage_size(false) + sizeof(bg::EvaluationTime));
}

TEST_CASE("graph structure distinguishes default fallbacks",
          "[feature:operator][feature:wiring][series:value][scalar:integer][shape:single-node]") {
    const auto structure = [](bg::i64 fallback) {
        auto wiring = bg::GraphWiring{};
        const auto context = bg::WiringContext{wiring};
        const auto source = bg::node<default_structure_source>();
        const auto sink = bg::node<default_structure_sink>();
        const auto input = source();
        const auto result = sink(bg::operators::default_(input, fallback));
        const auto boundaries = std::array<bg::WiringPoint, 1>{input};
        (void)wiring.complete_pending_wiring();
        return bg::details::graph_wiring_structure(wiring, boundaries, static_cast<bg::WiringPoint>(result));
    };

    CHECK_FALSE(structure(7) == structure(8));
}
