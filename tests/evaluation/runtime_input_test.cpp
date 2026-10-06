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

struct pending_evaluation_node {
    bool evaluate() const { return true; }
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
    const auto &value_runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    runtime.bind_runtime_input(0, value_runtime_type, left_state, &left_value, time(1));
    runtime.bind_runtime_input(1, value_runtime_type, right_state, &right_value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));

    REQUIRE(add_runtime_inputs::observations.size() == 1);
    CHECK(add_runtime_inputs::observations.back().left_modified);
    CHECK(add_runtime_inputs::observations.back().right_modified);
    CHECK(add_runtime_inputs::evaluation_times.back() == time(1));
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 42);

    // The source state is still stamped at time(1); rebinding must make the sampled input current at time(2).
    left_value = 15;
    runtime.bind_runtime_input(0, value_runtime_type, left_state, &left_value, time(2));
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
    const auto &value_runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    runtime.bind_runtime_input(0, value_runtime_type, source_state, &value, time(1));
    runtime.bind_runtime_input(1, value_runtime_type, source_state, &value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));
    CHECK(active_optional_runtime_input::evaluations == 1);
    CHECK(passive_optional_runtime_input::evaluations == 0);

    runtime.bind_runtime_input(0, value_runtime_type, source_state, &value, time(2));
    runtime.bind_runtime_input(1, value_runtime_type, source_state, &value, time(2));
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
    runtime.bind_runtime_input(0, bg::cpp_time_series_runtime_type<bg::TS<Value>>(), source_state, &value,
                               time(1));
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
    runtime.bind_runtime_input(0, bg::cpp_time_series_runtime_type<bg::TS<Value>>(), source_state, &value,
                               time(3));
    runtime.evaluate(time(3));
    CHECK(required_runtime_input::evaluations == 2);
    CHECK(runtime.node(0).output_last_updated().valid());
    CHECK(runtime.node(0).output_last_updated().became_valid());
    CHECK(*static_cast<const Value *>(runtime.node(0).output_value()) == 11);
}

TEST_CASE("runtime input changes must be delivered before their graph cycle",
          "[evaluation][feature:runtime-input][transition:stop][transition:update]") {
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
    const auto &value_runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    runtime.bind_runtime_input(0, value_runtime_type, source_state, &value, time(1));
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK_THROWS_WITH(runtime.bind_runtime_input(0, value_runtime_type, source_state, &value, time(1)),
                      "a runtime input change must follow the previous graph cycle");
    CHECK_THROWS_WITH(runtime.clear_runtime_input(0, time(1)),
                      "a runtime input change must follow the previous graph cycle");

    runtime.stop();
    CHECK_THROWS_WITH(runtime.bind_runtime_input(0, value_runtime_type, source_state, &value, time(2)),
                      "a stopped or finalized runtime graph cannot bind an input");
    runtime.finalize(global_state);
    CHECK_THROWS_WITH(runtime.clear_runtime_input(0, time(2)),
                      "a stopped or finalized runtime graph cannot clear an input");
}

TEST_CASE("runtime input records retain the 16-byte slot layout",
          "[feature:runtime-input][feature:graph-compilation][storage:fixed]") {
    CHECK(sizeof(bg::RuntimeInputBinding) == 16);
    CHECK(sizeof(bg::details::RuntimeInputDescriptor) == 24);
}

TEST_CASE("borrowed runtime graph views preserve validity between cycles",
          "[evaluation][feature:runtime-input][feature:graph-compilation][transition:invalidation]") {
    const auto consumer = bg::node<required_runtime_input>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)consumer(wiring.runtime_input(bg::TS<Value>::type()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.supports_borrowed_evaluation());

    auto storage = std::vector<std::byte>(bg::RuntimeGraph::borrowed_storage_size(compiled));
    bg::RuntimeGraph::initialize_borrowed_storage(compiled, storage);
    auto global_state = bg::GlobalState{};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto value = Value{9};
    auto previous_input_validity = bg::CompiledGraph::ScheduleWord{};

    {
        auto runtime = bg::RuntimeGraph{compiled, bg::allocator{}, storage, global_state};
        runtime.bind_runtime_input(0, bg::cpp_time_series_runtime_type<bg::TS<Value>>(), source_state, &value,
                                   time(1));
        runtime.evaluate(time(1), global_state);
        REQUIRE(runtime.node(0).output_last_updated().valid());
        previous_input_validity = runtime.input_validity();
    }

    {
        auto runtime = bg::RuntimeGraph{compiled, bg::allocator{}, storage, global_state, previous_input_validity};
        runtime.clear_runtime_input(0, time(2));
        runtime.evaluate(time(2), global_state);
        CHECK_FALSE(runtime.node(0).output_last_updated().valid());
        CHECK(runtime.node(0).output_last_updated().became_invalid());
    }
}

TEST_CASE("borrowed runtime graph views preserve spill-word input validity",
          "[evaluation][feature:runtime-input][feature:graph-compilation][storage:fixed]") {
    const auto consumer = bg::node<required_runtime_input>();
    auto wiring = bg::GraphWiring{};
    auto inputs = std::vector<bg::WiringPoint>{};
    {
        const auto context = bg::WiringContext{wiring};
        for (auto i = std::size_t{}; i < bg::CompiledGraph::schedule_word_bits + 1; ++i) {
            inputs.push_back(wiring.runtime_input(bg::TS<Value>::type()));
            (void)consumer(inputs.back());
        }
    }

    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.supports_borrowed_evaluation());
    REQUIRE(compiled.schedule_word_count() == 2);

    auto storage = std::vector<std::byte>(bg::RuntimeGraph::borrowed_storage_size(compiled));
    bg::RuntimeGraph::initialize_borrowed_storage(compiled, storage);
    auto global_state = bg::GlobalState{};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto values = std::vector<Value>(inputs.size(), Value{17});
    auto previous_input_validity = bg::CompiledGraph::ScheduleWord{};

    {
        auto runtime = bg::RuntimeGraph{compiled, bg::allocator{}, storage, global_state};
        for (auto i = std::size_t{}; i < inputs.size(); ++i) {
            runtime.bind_runtime_input(i, bg::cpp_time_series_runtime_type<bg::TS<Value>>(), source_state,
                                       &values[i], time(1));
        }
        runtime.evaluate(time(1), global_state);
        REQUIRE(runtime.node(bg::CompiledGraph::schedule_word_bits).output_last_updated().valid());
        previous_input_validity = runtime.input_validity();
    }

    {
        auto runtime = bg::RuntimeGraph{compiled, bg::allocator{}, storage, global_state, previous_input_validity};
        runtime.clear_runtime_input(inputs.size() - 1, time(2));
        runtime.evaluate(time(2), global_state);
        CHECK_FALSE(runtime.node(bg::CompiledGraph::schedule_word_bits).output_last_updated().valid());
        CHECK(runtime.node(bg::CompiledGraph::schedule_word_bits).output_last_updated().became_invalid());
    }
}

TEST_CASE("borrowed runtime graph instances can evaluate at the same time",
          "[evaluation][feature:runtime-input][feature:graph-compilation][shape:multiple][transition:first-tick]") {
    const auto consumer = bg::node<required_runtime_input>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)consumer(wiring.runtime_input(bg::TS<Value>::type()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.supports_borrowed_evaluation());
    auto first_storage = std::vector<std::byte>(bg::RuntimeGraph::borrowed_storage_size(compiled));
    auto second_storage = std::vector<std::byte>(bg::RuntimeGraph::borrowed_storage_size(compiled));
    bg::RuntimeGraph::initialize_borrowed_storage(compiled, first_storage);
    bg::RuntimeGraph::initialize_borrowed_storage(compiled, second_storage);

    auto global_state = bg::GlobalState{};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto first_value = Value{23};
    auto second_value = Value{47};
    auto first = bg::RuntimeGraph{compiled, bg::allocator{}, first_storage, global_state};
    auto second = bg::RuntimeGraph{compiled, bg::allocator{}, second_storage, global_state};
    first.bind_runtime_input(0, bg::cpp_time_series_runtime_type<bg::TS<Value>>(), source_state, &first_value,
                            time(1));
    second.bind_runtime_input(0, bg::cpp_time_series_runtime_type<bg::TS<Value>>(), source_state, &second_value,
                             time(1));

    first.evaluate(time(1), global_state);
    second.evaluate(time(1), global_state);
    CHECK(*static_cast<const Value *>(first.node(0).output_value()) == 23);
    CHECK(*static_cast<const Value *>(second.node(0).output_value()) == 47);
}

TEST_CASE("borrowed zero-node graphs expose a directly returned runtime input",
          "[evaluation][feature:runtime-input][feature:graph-compilation][shape:source]") {
    auto wiring = bg::GraphWiring{};
    auto result = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{wiring};
        result = wiring.runtime_input(bg::TS<Value>::type());
        (void)wiring.runtime_input(bg::TS<Value>::type());
    }

    const auto result_input_index = wiring.runtime_input_index(result);
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.node_count() == 0);
    REQUIRE(compiled.supports_borrowed_evaluation());

    auto storage = std::vector<std::byte>(bg::RuntimeGraph::borrowed_storage_size(compiled));
    bg::RuntimeGraph::initialize_borrowed_storage(compiled, storage);
    auto global_state = bg::GlobalState{};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto result_value = Value{31};
    auto unused_value = Value{99};
    auto runtime = bg::RuntimeGraph{compiled, bg::allocator{}, storage, global_state};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    runtime.bind_runtime_input(result_input_index, runtime_type, source_state, &result_value, time(1));
    runtime.bind_runtime_input(1, runtime_type, source_state, &unused_value, time(1));
    runtime.evaluate(time(1), global_state);

    const auto &result_binding = *static_cast<const bg::RuntimeInputBinding *>(
        runtime.storage_at(compiled.runtime_inputs()[result_input_index].slot_offset));
    CHECK(result_binding.last_updated.valid());
    CHECK(result_binding.last_updated.time() == time(1));
    CHECK(*static_cast<const Value *>(result_binding.value) == 31);
}

TEST_CASE("borrowed runtime views validate storage and runtime representation",
          "[evaluation][feature:runtime-input][feature:graph-compilation]") {
    const auto consumer = bg::node<required_runtime_input>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)consumer(wiring.runtime_input(bg::TS<Value>::type()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.supports_borrowed_evaluation());
    const auto storage_size = bg::RuntimeGraph::borrowed_storage_size(compiled);
    auto storage = std::vector<std::byte>(storage_size + 1);
    auto global_state = bg::GlobalState{};
    CHECK_THROWS_WITH(bg::RuntimeGraph::initialize_borrowed_storage(compiled,
                                                                    std::span{storage.data(), storage_size - 1}),
                      "borrowed runtime storage is too small");
    CHECK_THROWS_WITH(bg::RuntimeGraph::initialize_borrowed_storage(compiled,
                                                                    std::span{storage.data() + 1, storage_size}),
                      "borrowed runtime storage is misaligned");

    bg::RuntimeGraph::initialize_borrowed_storage(compiled, std::span{storage.data(), storage_size});
    auto runtime = bg::RuntimeGraph{compiled, bg::allocator{}, std::span{storage.data(), storage_size}, global_state};
    auto source_state = bg::TimeSeriesState{};
    source_state.publish(time(1));
    auto value = Value{11};
    CHECK_THROWS_WITH(runtime.bind_runtime_input(0, bg::cpp_time_series_runtime_type<bg::TS<double>>(), source_state,
                                                 &value, time(1)),
                      "a runtime input value has a different runtime type than the binding expects");

    auto pending_wiring = bg::GraphWiring{};
    const auto pending_node = bg::node<pending_evaluation_node>();
    {
        const auto context = bg::WiringContext{pending_wiring};
        (void)pending_node();
    }
    const auto pending_graph = bg::CompiledGraph{pending_wiring};
    CHECK_FALSE(pending_graph.supports_borrowed_evaluation());
    CHECK_THROWS_WITH(bg::RuntimeGraph::initialize_borrowed_storage(
                          pending_graph, std::span{storage.data(), storage_size}),
                      "the compiled graph does not support borrowed evaluation");
}
