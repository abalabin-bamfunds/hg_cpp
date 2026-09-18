#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/node/node_template.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
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
    bg::In<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>> input;

    void evaluate() const {
        ++evaluations;
        valid = input.valid();
        value = valid ? *input : 0;
    }

    inline static int evaluations{};
    inline static bool valid{};
    inline static std::int64_t value{};
};

struct constant_reference_pass {
    bg::In<bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>> input;
    bg::Out<bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>> output;

    void evaluate() const { output.copy(input); }
};

struct constant_required_sum {
    bg::In<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>> constant;
    bg::In<bg::TS<std::int64_t>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const {
        ++evaluations;
        output.set(*constant + *input);
    }

    inline static int evaluations{};
};

struct dynamic_constant_sink {
    void evaluate() const { ++evaluations; }

    inline static int evaluations{};
};

struct constant_timestamp_sink {
    bg::In<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>> input;
    bg::Out<bg::TS<std::int64_t>> output;

    void evaluate() const { output.set((input.last_updated() - bg::MIN_DT).count()); }
};

class dynamic_constant_subscriber final : public bg::NodeTemplate {
  public:
    const bg::Signature &signature() const override {
        static const auto signature = bg::Signature{
            bg::util::symbol{"dynamic_constant_subscriber"},
            {{bg::util::symbol{"input"}, &bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>::type()}},
        };
        return signature;
    }

    bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override { return nullptr; }

    bg::EvaluationFunction start_function(const bg::ResolvedSignature &) const noexcept override { return &start; }

    bg::NodeStateLayout state_layout(const bg::ResolvedSignature &, std::span<const bg::ScalarRuntimeType *const>,
                                     std::span<const bg::ScalarType *const>) const override {
        return {.size = sizeof(bg::details::DynamicSubscribers *),
                .alignment = alignof(bg::details::DynamicSubscribers *),
                .field_offsets = {},
                .generic_runtime_types = {}};
    }

    void construct_initial_state(const bg::NodeStateLayout &, void *storage) const noexcept override {
        std::construct_at(static_cast<bg::details::DynamicSubscribers **>(storage), nullptr);
    }

    bg::EvaluationFunction state_destruction_function(const bg::ResolvedSignature &) const noexcept override {
        return &destroy_state;
    }

    std::optional<bg::DynamicSubscribersLayout>
    dynamic_subscribers_layout(const bg::ResolvedSignature &) const noexcept override {
        return bg::DynamicSubscribersLayout{.source_input = 0, .state_offset = 0};
    }

  private:
    static void start(const bg::CompiledNode &node, void *runtime_storage, bg::EvaluationContext *context) {
        auto *subscribers = static_cast<bg::details::DynamicSubscribers **>(bg::Node{node, runtime_storage}.state());
        bg::details::DynamicSubscribers::ensure(*subscribers, context->runtime_graph().allocation())
            .add(context->runtime_graph(), 0, 0, 1, true, false);
    }

    static void destroy_state(const bg::CompiledNode &node, void *runtime_storage, bg::EvaluationContext *) noexcept {
        auto *subscribers = static_cast<bg::details::DynamicSubscribers **>(bg::Node{node, runtime_storage}.state());
        bg::details::DynamicSubscribers::release(*subscribers);
    }
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
    constant_reference_sink::valid = false;
    constant_reference_sink::value = 0;
    auto wiring = bg::GraphWiring{};
    const auto pass = bg::node<constant_reference_pass>();
    const auto sink = bg::node<constant_reference_sink>();
    {
        auto context = bg::WiringContext{wiring};
        sink(pass(const_(9).as<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node_count() == 3);

    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(constant_reference_sink::evaluations == 1);
    CHECK(constant_reference_sink::valid);
    CHECK(constant_reference_sink::value == 9);
}

TEST_CASE("a constant waits for another required input",
          "[feature:constant][feature:scheduling][feature:validity][transition:revalidation]") {
    const auto sum = bg::node<constant_required_sum>();
    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) {
        return sum(bg::operators::const_(4), input);
    };

    constant_required_sum::evaluations = 0;
    const auto actual = bg::testing::test_graph(graph, std::array<std::optional<std::int64_t>, 2>{std::nullopt, 3});

    CHECK(actual.equals({std::nullopt, 7}));
    CHECK(constant_required_sum::evaluations == 1);
}

TEST_CASE("a constant notifies dynamic subscribers at start",
          "[feature:constant][feature:scheduling][transition:start]") {
    using namespace bg::operators;

    dynamic_constant_sink::evaluations = 0;
    const auto source = dynamic_constant_subscriber{};
    const auto sink = bg::node<dynamic_constant_sink>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        source(const_(1).as<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::const_>>());
        sink();
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(dynamic_constant_sink::evaluations == 1);
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
