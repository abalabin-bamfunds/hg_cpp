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
#include <string>

namespace {

struct constant_sink {
    bg::In<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>> input;

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluations;
        value = *input;
        last_updated = input.last_updated();
        modified = input.modified();
        evaluation_time = context->evaluation_time();
    }

    inline static int evaluations{};
    inline static std::int64_t value{};
    inline static bg::EvaluationTime last_updated{bg::MIN_DT};
    inline static bg::EvaluationTime evaluation_time{bg::MIN_DT};
    inline static bool modified{};
};

struct passive_constant_sink {
    bg::In<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>, bg::passive> input;

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
    bg::In<bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>> input;

    void evaluate() const { ++evaluations; }

    inline static int evaluations{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
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
        const auto value = const_(42).as<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>();
        sink(value);
    }

    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 1);
    REQUIRE(compiled.node(0).parameters()[0].mode() == bg::ParameterBindingMode::constant);
    CHECK(sizeof(bg::ParameterBinding) == 8);
    CHECK(sizeof(bg::Node) == 2 * sizeof(void *));

    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(constant_sink::evaluations == 1);
    CHECK(constant_sink::value == 42);
    CHECK(constant_sink::last_updated == time(1));
    CHECK(constant_sink::evaluation_time == time(1));
    CHECK(constant_sink::modified);

    runtime.evaluate(time(2));
    CHECK(constant_sink::evaluations == 1);

    auto second = bg::RuntimeGraph{compiled};
    second.start(time(3));
    second.evaluate(time(3));
    CHECK(constant_sink::last_updated == time(3));
}

TEST_CASE("a constant value updates only in the first test graph cycle",
          "[feature:constant][feature:test-graph][series:value][scalar:integer][guarantee:const]"
          "[shape:single-node][transition:first-tick]") {
    const auto graph = [](bg::wp<bg::TS<std::int64_t>>) {
        return bg::operators::const_(3).as<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>();
    };

    CHECK(bg::testing::test_graph(graph, std::array<std::int64_t, 2>{1, 2}).equals({3, std::nullopt}));
}

TEST_CASE("one folded constant serves active and passive consumers",
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
        const auto value = const_(7).as<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>();
        active_sink(value);
        passive_sink(value);
    }

    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);

    auto runtime = bg::RuntimeGraph{compiled};
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

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(string_constant_sink::value == "fixed");
    CHECK(string_constant_sink::last_updated == time(1));
}

TEST_CASE("a constant value-to-reference binding keeps the reference node physical",
          "[feature:constant][feature:reference][feature:graph-compilation][series:reference][series:value]"
          "[binding:value-to-reference][scalar:integer][guarantee:const][shape:chain][transition:start]") {
    using namespace bg::operators;

    constant_reference_sink::evaluations = 0;
    auto wiring = bg::GraphWiring{};
    const auto sink = bg::node<constant_reference_sink>();
    {
        auto context = bg::WiringContext{wiring};
        sink(const_(9).as<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>());
    }

    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);

    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(constant_reference_sink::evaluations == 1);
}
