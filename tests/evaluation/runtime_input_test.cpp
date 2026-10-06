#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace {

using Value = std::int64_t;

bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct InputObservation {
    bool left_modified;
    bool right_modified;
    bg::EvaluationTime left_time;
    bg::EvaluationTime right_time;
};

struct add_runtime_inputs {
    bg::In<bg::TS<Value>> left;
    bg::In<bg::TS<Value>> right;
    bg::Out<bg::TS<Value>> output;

    void evaluate(bg::EvaluationContext *context) const {
        observations.push_back({left.modified(), right.modified(), left.last_updated(), right.last_updated()});
        output.set(*left + *right);
        evaluation_times.push_back(context->evaluation_time());
    }

    inline static std::vector<InputObservation> observations;
    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

struct active_optional_runtime_input {
    bg::In<bg::TS<Value>, bg::optional> input;
    void evaluate() const { ++evaluations; }
    inline static std::size_t evaluations{};
};

struct passive_optional_runtime_input {
    bg::In<bg::TS<Value>, bg::passive, bg::optional> input;
    void evaluate() const { ++evaluations; }
    inline static std::size_t evaluations{};
};

struct required_runtime_input {
    bg::In<bg::TS<Value>> input;
    bg::Out<bg::TS<Value>> output;

    void evaluate() const {
        ++evaluations;
        output.set(*input);
    }

    inline static std::size_t evaluations{};
};

struct runtime_input_reference_source {
    static constexpr bool schedule_on_start = true;

    bg::Out<bg::TS<Value>> output;
    void evaluate() const { output.set(23); }
};

struct runtime_input_reference_source_b {
    static constexpr bool schedule_on_start = true;

    bg::Out<bg::TS<Value>> output;
    void evaluate() const { output.set(47); }
};

} // namespace

TEST_CASE("runtime inputs sample distinct values and restamp older bindings on rebind",
          "[evaluation][feature:runtime-input][series:value][binding:direct][shape:single-node]"
          "[transition:first-tick][transition:update]") {
    const auto addition = bg::node<add_runtime_inputs>();
    auto wiring = bg::GraphWiring{};
    auto left = bg::WiringPoint{};
    auto right = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{wiring};
        left = wiring.runtime_input(bg::TS<Value>::type());
        right = wiring.runtime_input(bg::TS<Value>::type());
        (void)addition(left, right);
    }

    REQUIRE(wiring.runtime_input_index(left) == 0);
    REQUIRE(wiring.runtime_input_index(right) == 1);
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.runtime_input_count() == 2);
    CHECK(compiled.runtime_inputs()[0].slot_offset != compiled.runtime_inputs()[1].slot_offset);
    CHECK(compiled.node(0).parameters()[0].mode() == bg::ParameterBindingMode::runtime_input);
    CHECK(compiled.node(0).parameters()[1].mode() == bg::ParameterBindingMode::runtime_input);

    auto left_state = bg::TimeSeriesState{};
    auto right_state = bg::TimeSeriesState{};
    left_state.publish(time(1));
    right_state.publish(time(1));
    auto left_value = Value{10};
    auto right_value = Value{32};

    add_runtime_inputs::observations.clear();
    add_runtime_inputs::evaluation_times.clear();
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.bind_runtime_input(0, left_state, &left_value, time(1));
    runtime.bind_runtime_input(1, right_state, &right_value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));

    REQUIRE(add_runtime_inputs::observations.size() == 1);
    CHECK(add_runtime_inputs::observations.back().left_modified);
    CHECK(add_runtime_inputs::observations.back().right_modified);
    CHECK(add_runtime_inputs::evaluation_times.back() == time(1));
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 42);

    // The source state is still stamped at time(1); rebinding must make the sampled input current at time(2).
    left_value = 15;
    runtime.bind_runtime_input(0, left_state, &left_value, time(2));
    runtime.evaluate(time(2));

    REQUIRE(add_runtime_inputs::observations.size() == 2);
    CHECK(add_runtime_inputs::observations.back().left_modified);
    CHECK_FALSE(add_runtime_inputs::observations.back().right_modified);
    CHECK(add_runtime_inputs::observations.back().left_time == time(2));
    CHECK(add_runtime_inputs::observations.back().right_time == time(1));
    CHECK(add_runtime_inputs::evaluation_times.back() == time(2));
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 47);
}

TEST_CASE("runtime input notifications preserve active and passive scheduling",
          "[evaluation][feature:runtime-input][feature:input-activity][series:value]"
          "[activity:active][activity:passive][validity:optional][transition:update]") {
    const auto active = bg::node<active_optional_runtime_input>();
    const auto passive = bg::node<passive_optional_runtime_input>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)active(wiring.runtime_input(bg::TS<Value>::type()));
        (void)passive(wiring.runtime_input(bg::TS<Value>::type()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.runtime_inputs().size() == 2);
    REQUIRE(compiled.runtime_input_subscribers(compiled.runtime_inputs()[0]).size() == 1);
    CHECK(compiled.runtime_input_subscribers(compiled.runtime_inputs()[0])[0].active());
    CHECK(compiled.runtime_input_subscribers(compiled.runtime_inputs()[0])[0].active_optional());
    CHECK(compiled.runtime_input_subscribers(compiled.runtime_inputs()[1]).empty());

    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto value = Value{7};
    active_optional_runtime_input::evaluations = 0;
    passive_optional_runtime_input::evaluations = 0;
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.bind_runtime_input(0, source_state, &value, time(1));
    runtime.bind_runtime_input(1, source_state, &value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(active_optional_runtime_input::evaluations == 1);
    CHECK(passive_optional_runtime_input::evaluations == 0);

    runtime.bind_runtime_input(0, source_state, &value, time(2));
    runtime.bind_runtime_input(1, source_state, &value, time(2));
    runtime.evaluate(time(2));
    CHECK(active_optional_runtime_input::evaluations == 2);
    CHECK(passive_optional_runtime_input::evaluations == 0);
}

TEST_CASE("clearing a required runtime input invalidates its output until revalidation",
          "[evaluation][feature:runtime-input][feature:validity][series:value]"
          "[validity:required][transition:invalidation][transition:revalidation]") {
    const auto sink = bg::node<required_runtime_input>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)sink(wiring.runtime_input(bg::TS<Value>::type()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto value = Value{9};
    required_runtime_input::evaluations = 0;
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.bind_runtime_input(0, source_state, &value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));
    REQUIRE(required_runtime_input::evaluations == 1);
    REQUIRE(runtime.node(0).output_last_updated().valid());

    runtime.clear_runtime_input(0, time(2));
    runtime.evaluate(time(2));
    CHECK(required_runtime_input::evaluations == 1);
    CHECK_FALSE(runtime.node(0).output_last_updated().valid());
    CHECK(runtime.node(0).output_last_updated().became_invalid());

    value = 11;
    runtime.bind_runtime_input(0, source_state, &value, time(3));
    runtime.evaluate(time(3));
    CHECK(required_runtime_input::evaluations == 2);
    CHECK(runtime.node(0).output_last_updated().valid());
    CHECK(runtime.node(0).output_last_updated().became_valid());
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 11);
}

TEST_CASE("runtime input changes must be delivered before their graph cycle",
          "[evaluation][feature:runtime-input][lifecycle:stop][transition:update]") {
    const auto consumer = bg::node<active_optional_runtime_input>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)consumer(wiring.runtime_input(bg::TS<Value>::type()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto value = Value{9};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.bind_runtime_input(0, source_state, &value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK_THROWS_WITH(runtime.bind_runtime_input(0, source_state, &value, time(1)),
                      "a runtime input change must follow the previous graph cycle");
    CHECK_THROWS_WITH(runtime.clear_runtime_input(0, time(1)),
                      "a runtime input change must follow the previous graph cycle");

    runtime.stop();
    CHECK_THROWS_WITH(runtime.bind_runtime_input(0, source_state, &value, time(2)),
                      "a stopped or finalized runtime graph cannot bind an input");
    runtime.finalize(global_state);
    CHECK_THROWS_WITH(runtime.clear_runtime_input(0, time(2)),
                      "a stopped or finalized runtime graph cannot clear an input");
}

TEST_CASE("reference runtime inputs use ordinary reference binding and notification",
          "[evaluation][feature:runtime-input][feature:reference][series:value]"
          "[binding:reference-to-value][shape:multiple][transition:first-tick][transition:rebind]") {
    const auto source_node = bg::node<runtime_input_reference_source>();
    const auto source_node_b = bg::node<runtime_input_reference_source_b>();
    auto source_wiring = bg::GraphWiring{};
    auto source_value = bg::WiringPoint{};
    auto reference_output = bg::WiringPoint{};
    auto reference_output_b = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{source_wiring};
        source_value = source_node();
        const auto value_b = source_node_b();
        const auto &reference_type =
            bg::time_series_reference_type(bg::TS<Value>::type(), bg::TimeSeriesGuarantee::const_);
        reference_output = source_wiring.materialize(source_value, reference_type);
        reference_output_b = source_wiring.materialize(value_b, reference_type);
    }

    const auto source_compiled = bg::CompiledGraph{source_wiring};
    auto source_global_state = bg::GlobalState{};
    auto source = bg::RuntimeGraph{source_compiled, source_global_state};

    const auto consumer = bg::node<required_runtime_input>();
    auto consumer_wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{consumer_wiring};
        const auto &external_reference_type =
            bg::time_series_reference_type(bg::TS<Value>::type(), bg::TimeSeriesGuarantee::ordinary);
        (void)consumer(consumer_wiring.runtime_input(external_reference_type));
    }

    const auto consumer_compiled = bg::CompiledGraph{consumer_wiring};
    REQUIRE(consumer_compiled.node(0).parameters()[0].mode() == bg::ParameterBindingMode::runtime_reference);
    REQUIRE(consumer_compiled.runtime_input_notification(consumer_compiled.runtime_inputs()[0]) != nullptr);

    const auto evaluation_time = time(1);
    source.start(evaluation_time);
    source.evaluate(evaluation_time);
    const auto reference_node_index = source_wiring.output_node_index(reference_output);
    const auto reference_node_index_b = source_wiring.output_node_index(reference_output_b);
    const auto reference_source = source.node(reference_node_index);
    const auto reference_source_b = source.node(reference_node_index_b);

    auto consumer_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{consumer_compiled, consumer_global_state};
    runtime.bind_runtime_input(0, reference_source.output_last_updated(), reference_source.output_value(),
                               evaluation_time);
    required_runtime_input::evaluations = 0;
    runtime.start(evaluation_time);
    runtime.evaluate(evaluation_time);

    CHECK(required_runtime_input::evaluations == 1);
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 23);

    const auto clear_time = time(2);
    runtime.clear_runtime_input(0, clear_time);
    runtime.evaluate(clear_time);
    CHECK_FALSE(runtime.node(0).output_last_updated().valid());
    CHECK(runtime.node(0).output_last_updated().became_invalid());

    const auto rebind_time = time(3);
    runtime.bind_runtime_input(0, reference_source_b.output_last_updated(), reference_source_b.output_value(),
                               rebind_time);
    runtime.evaluate(rebind_time);
    CHECK(required_runtime_input::evaluations == 2);
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 47);

    // Re-evaluating source A must not schedule the consumer after its reference subscription was removed.
    source.schedule(source_wiring.output_node_index(source_value));
    const auto source_update_time = time(4);
    source.evaluate(source_update_time);
    runtime.evaluate(source_update_time);
    CHECK(required_runtime_input::evaluations == 2);
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 47);

    runtime.stop();
    source.stop();
}

TEST_CASE("runtime input records retain the 16-byte slot layout",
          "[feature:runtime-input][feature:graph-compilation][storage:fixed]") {
    CHECK(sizeof(bg::RuntimeInputBinding) == 16);
    CHECK(sizeof(bg::details::RuntimeInputDescriptor) == 24);
}
