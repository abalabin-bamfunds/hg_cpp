#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>

namespace {

struct constant_sink {
    bg::In<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> input;

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

struct passive_constant_sink {
    bg::In<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::passive> input;

    void evaluate() const { ++evaluations; }

    inline static int evaluations{};
};

struct string_constant_sink {
    bg::In<bg::TS<bg::string, bg::TimeSeriesGuarantee::const_>> input;

    void evaluate() const {
        value = input.value();
        last_updated = input.last_updated();
    }

    inline static std::string value;
    inline static bg::EvaluationTime last_updated{bg::MIN_DT};
};

struct constant_reference_sink {
    bg::In<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> input;

    void evaluate() const {
        ++evaluations;
        valid = input.valid();
        value = valid ? *input : 0;
    }

    inline static int evaluations{};
    inline static bool valid{};
    inline static bg::i64 value{};
};

struct constant_reference_pass {
    bg::In<bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::const_>> input;
    bg::Out<bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::const_>> output;

    void evaluate() const { output.copy(input); }
};

struct constant_required_sum {
    bg::In<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> constant;
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        ++evaluations;
        output.set(*constant + *input);
    }

    inline static int evaluations{};
};

struct constant_timestamp_sink {
    bg::In<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set((input.last_updated() - bg::MIN_DT).count()); }
};

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a scalar constant binds directly to its active consumer",
          "[feature:constant][feature:graph-compilation][feature:scheduling][series:value][scalar:integer]"
          "[guarantee:const][activity:active][validity:required][transition:first-tick]") {
    using namespace bg::operators;

    constant_sink::evaluations = 0;
    auto wiring = bg::GraphWiring{};
    const auto sink = bg::node<constant_sink>();
    {
        auto context = bg::WiringContext{wiring};
        const auto value = const_(42).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        sink(value);
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    REQUIRE(compiled.node(0).parameters()[0].mode() == bg::ParameterBindingMode::constant);
    CHECK(sizeof(bg::ParameterBinding) == 8);
    CHECK(sizeof(bg::Node) == 2 * sizeof(void *));

    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(constant_sink::evaluations == 1);
    CHECK(constant_sink::value == 42);
    CHECK(constant_sink::last_updated == time(1));
    CHECK(constant_sink::evaluation_time == time(1));
    CHECK(constant_sink::modified);

    runtime.evaluate(time(2));
    CHECK(constant_sink::evaluations == 1);

    auto second_global_state = bg::GlobalState{};
    auto second = bg::RuntimeGraph{compiled, second_global_state};
    second.start(time(3));
    second.evaluate(time(3));
    CHECK(constant_sink::last_updated == time(3));
}

TEST_CASE("a constant value updates only in the first test graph cycle",
          "[feature:constant][feature:test-graph][series:value][scalar:integer][guarantee:const]"
          "[shape:single-node][transition:first-tick]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>>) {
        return bg::operators::const_(3).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{1, 2}).equals({3, std::nullopt}));
}

TEST_CASE("one constant serves active and passive consumers",
          "[feature:constant][feature:graph-compilation][feature:input-activity][feature:scheduling][series:value]"
          "[scalar:integer][guarantee:const][shape:fan-out][activity:active][activity:passive]") {
    using namespace bg::operators;

    constant_sink::evaluations = 0;
    passive_constant_sink::evaluations = 0;
    auto wiring = bg::GraphWiring{};
    const auto active_sink = bg::node<constant_sink>();
    const auto passive_sink = bg::node<passive_constant_sink>();
    {
        auto context = bg::WiringContext{wiring};
        const auto value = const_(7).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        active_sink(value);
        passive_sink(value);
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);

    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(constant_sink::evaluations == 1);
    CHECK(passive_constant_sink::evaluations == 0);
}

TEST_CASE("a string constant uses compiled scalar storage",
          "[feature:constant][feature:graph-compilation][series:value][scalar:string][storage:variable]"
          "[guarantee:const][shape:single-node][transition:first-tick]") {
    using namespace bg::operators;

    auto wiring = bg::GraphWiring{};
    const auto sink = bg::node<string_constant_sink>();
    {
        auto context = bg::WiringContext{wiring};
        sink(const_("fixed").as<bg::TS<bg::string, bg::TimeSeriesGuarantee::const_>>());
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(string_constant_sink::value == "fixed");
    CHECK(string_constant_sink::last_updated == time(1));
}

TEST_CASE("a constant value-to-reference binding keeps the reference node in the compiled graph",
          "[feature:constant][feature:reference][feature:graph-compilation][series:reference][series:value]"
          "[binding:value-to-reference][scalar:integer][guarantee:const][shape:chain][transition:start]") {
    using namespace bg::operators;

    constant_reference_sink::evaluations = 0;
    constant_reference_sink::valid = false;
    constant_reference_sink::value = 0;
    auto wiring = bg::GraphWiring{};
    const auto pass = bg::node<constant_reference_pass>();
    const auto sink = bg::node<constant_reference_sink>();
    {
        auto context = bg::WiringContext{wiring};
        sink(pass(const_(9).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>()));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node_count() == 3);

    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(constant_reference_sink::evaluations == 1);
    CHECK(constant_reference_sink::valid);
    CHECK(constant_reference_sink::value == 9);
}

TEST_CASE("a constant waits for another required input",
          "[feature:constant][feature:scheduling][feature:validity][transition:revalidation]") {
    const auto sum = bg::node<constant_required_sum>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return sum(bg::operators::const_(4), input);
    };

    constant_required_sum::evaluations = 0;
    const auto actual = bg::testing::test_graph(graph, std::array<std::optional<bg::i64>, 2>{std::nullopt, 3});

    CHECK(actual.equals({std::nullopt, 7}));
    CHECK(constant_required_sum::evaluations == 1);
}

TEST_CASE("an inner constant uses its own start time",
          "[feature:constant][feature:nested-graph][feature:scheduling][transition:start]") {
    using namespace bg::literals;

    const auto timestamp = bg::node<constant_timestamp_sink>();
    const auto inner = bg::graph(
        "name"_n = "constant_timestamp", [&] { return timestamp(bg::operators::const_(1)); });
    const auto outer = [&](bg::wp<bg::TS<bool>> run) { return bg::nested(inner, run); };

    const auto actual = bg::testing::test_graph(outer, std::array{false, true});

    CHECK(actual.equals({std::nullopt, 2}));
}
