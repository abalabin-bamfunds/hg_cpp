#include <bg/evaluation_profile.hpp>
#include <bg/global_state.hpp>
#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/nested.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace {

using namespace bg::literals;

struct profile_source {
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const {
        output.set(++evaluations);
    }

    inline static std::int64_t evaluations{};
};

struct profile_start_only {
    void start(bg::EvaluationContext *context) const {
        ++starts;
        context->schedule_now();
    }

    inline static std::size_t starts{};
};

struct throwing_profile_node {
    void start(bg::EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const { throw std::runtime_error{"profile test evaluation failure"}; }
    void stop() const noexcept { ++stops; }

    inline static std::size_t stops{};
};

const auto profile_increment = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
    output.set(*input + 1);
});

bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("runtime graph profiling records node and cycle checkpoints",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{256};
    profile_source::evaluations = 0;

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.stop(profile);
    profile.finish();

    CHECK_FALSE(profile.failed());
    CHECK(profile.recorded_bytes() > 16);

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    CHECK((reader.clock() == bg::EvaluationProfileClock::timestamp_counter ||
           reader.clock() == bg::EvaluationProfileClock::steady_clock_nanoseconds));

    auto graph_start_count = std::size_t{};
    auto graph_stop_count = std::size_t{};
    auto cycle_start_count = std::size_t{};
    auto cycle_end_count = std::size_t{};
    auto node_count = std::size_t{};
    auto definition_count = std::size_t{};
    auto node_metadata_count = std::size_t{};
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<bg::EvaluationProfileGraph>(&*record)) {
            ++graph_start_count;
            CHECK(graph->graph_id != 0);
            CHECK(graph->definition_id != 0);
            CHECK(graph->parent_graph_id == 0);
            CHECK(graph->owner_node_index == UINT32_MAX);
        } else if (std::holds_alternative<bg::EvaluationProfileGraphStop>(*record)) {
            ++graph_stop_count;
        } else if (const auto *cycle = std::get_if<bg::EvaluationProfileCycle>(&*record)) {
            if (cycle->boundary == bg::EvaluationProfileCycle::Boundary::start) {
                ++cycle_start_count;
                CHECK(cycle->evaluation_time == time(2));
            } else {
                ++cycle_end_count;
                CHECK(cycle->evaluation_time == time(2));
            }
        } else if (std::holds_alternative<bg::EvaluationProfileNode>(*record)) {
            ++node_count;
        } else if (std::holds_alternative<bg::EvaluationProfileDefinition>(*record)) {
            ++definition_count;
        } else if (std::holds_alternative<bg::EvaluationProfileNodeMetadata>(*record)) {
            ++node_metadata_count;
        }
    }

    CHECK(graph_start_count == 1);
    CHECK(graph_stop_count == 1);
    CHECK(cycle_start_count == 1);
    CHECK(cycle_end_count == 1);
    CHECK(node_count == 1);
    CHECK(definition_count == 1);
    CHECK(node_metadata_count == compiled.node_count());
    CHECK(profile_source::evaluations == 1);
}

TEST_CASE("runtime graph profiling records nested graph ownership",
          "[evaluation][profile][feature:profiling][feature:nested-graph]") {
    const auto inner = bg::graph("name"_n = "profile_inner", [](bg::wp<bg::TS<bg::i64>> input) {
        return profile_increment(input);
    });
    const auto outer = bg::graph("name"_n = "profile_outer", [&] {
        return bg::nested(inner, bg::operators::const_(true), bg::operators::const_(3));
    });

    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)outer();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{256};

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    auto root_graph_id = std::uint32_t{};
    auto nested_graph_count = std::size_t{};
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<bg::EvaluationProfileGraph>(&*record)) {
            if (graph->parent_graph_id == 0) {
                root_graph_id = graph->graph_id;
                CHECK(graph->owner_node_index == UINT32_MAX);
                continue;
            }

            ++nested_graph_count;
            CHECK(graph->parent_graph_id == root_graph_id);
            CHECK(graph->owner_node_index != UINT32_MAX);
        }
    }

    CHECK(root_graph_id != 0);
    CHECK(nested_graph_count == 1);
}

TEST_CASE("runtime graph profiling records scheduled nodes without evaluation functions",
          "[evaluation][profile][feature:profiling]") {
    const auto scheduled = bg::node<profile_start_only>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)scheduled();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{};
    profile_start_only::starts = 0;

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    auto node_count = std::size_t{};
    while (const auto record = reader.read()) {
        node_count += static_cast<std::size_t>(std::holds_alternative<bg::EvaluationProfileNode>(*record));
    }

    CHECK(node_count == 1);
    CHECK(profile_start_only::starts == 1);
}

TEST_CASE("simulation evaluation engine records a profile",
          "[evaluation][profile][feature:profiling][feature:simulation]") {
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{};
    profile_source::evaluations = 0;

    bg::SimulationEvaluationEngine{}.run(runtime, time(1), profile);
    profile.finish();

    CHECK_FALSE(profile.failed());
    CHECK(profile_source::evaluations == 1);

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    auto cycle_start_count = std::size_t{};
    auto cycle_end_count = std::size_t{};
    auto graph_stop_count = std::size_t{};
    while (const auto record = reader.read()) {
        if (const auto *cycle = std::get_if<bg::EvaluationProfileCycle>(&*record)) {
            cycle_start_count += cycle->boundary == bg::EvaluationProfileCycle::Boundary::start;
            cycle_end_count += cycle->boundary == bg::EvaluationProfileCycle::Boundary::end;
        } else if (std::holds_alternative<bg::EvaluationProfileGraphStop>(*record)) {
            ++graph_stop_count;
        }
    }

    CHECK(cycle_start_count == 1);
    CHECK(cycle_end_count == 1);
    CHECK(graph_stop_count == 1);
}

TEST_CASE("runtime graph profiling exposes committed cycles and releases consumed blocks",
          "[evaluation][profile][feature:profiling]") {
    auto memory = counting_memory_resource{};
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{64, bg::allocator{memory}};
    profile_source::evaluations = 0;

    runtime.start(time(1), profile);
    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    CHECK_FALSE(reader.read().has_value());

    runtime.evaluate(time(2), profile);
    CHECK(memory.allocations() > 1);

    const auto allocated_bytes_before_drain = memory.allocated_bytes();
    auto record_count = std::size_t{};
    while (reader.read()) {
        ++record_count;
    }
    CHECK(record_count > 4);
    CHECK(memory.allocated_bytes() < allocated_bytes_before_drain);

    runtime.stop(profile);
    profile.finish();
    auto graph_stop_seen = false;
    while (const auto record = reader.read()) {
        graph_stop_seen |= std::holds_alternative<bg::EvaluationProfileGraphStop>(*record);
    }
    CHECK(graph_stop_seen);
}

TEST_CASE("runtime graph profiling reuses a compiled graph definition while instances are active",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto first = bg::RuntimeGraph{compiled, global_state};
    auto second = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{};
    profile_source::evaluations = 0;

    first.start(time(1), profile);
    second.start(time(1), profile);
    first.evaluate(time(2), profile);
    second.evaluate(time(3), profile);
    first.stop(profile);
    second.stop(profile);
    profile.finish();

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    auto definition_count = std::size_t{};
    auto graph_start_count = std::size_t{};
    std::uint32_t first_definition = 0;
    std::uint32_t shared_definition = 0;
    while (const auto record = reader.read()) {
        if (const auto *definition = std::get_if<bg::EvaluationProfileDefinition>(&*record)) {
            ++definition_count;
            first_definition = definition->definition_id;
        } else if (const auto *graph = std::get_if<bg::EvaluationProfileGraph>(&*record)) {
            ++graph_start_count;
            CHECK(graph->definition_id == first_definition);
            if (shared_definition == 0) {
                shared_definition = graph->definition_id;
            } else {
                CHECK(graph->definition_id == shared_definition);
            }
        }
    }

    CHECK(definition_count == 1);
    CHECK(graph_start_count == 2);
}

TEST_CASE("runtime graph profiling falls back to plain evaluation after recording allocation failure",
          "[evaluation][profile][feature:profiling]") {
    auto memory = counting_memory_resource{};
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{32, bg::allocator{memory}};
    memory.fail_after(0);
    profile_source::evaluations = 0;

    runtime.start(time(1), profile);
    CHECK(profile.failed());
    runtime.evaluate(time(2), profile);
    CHECK(profile_source::evaluations == 1);
    runtime.stop(profile);
    profile.finish();
}

TEST_CASE("runtime graph profiling falls back to plain evaluation after cycle recording allocation failure",
          "[evaluation][profile][feature:profiling]") {
    auto memory = counting_memory_resource{};
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{32, bg::allocator{memory}};
    profile_source::evaluations = 0;

    runtime.start(time(1), profile);
    CHECK_FALSE(profile.failed());
    memory.fail_after(0);
    runtime.evaluate(time(2), profile);

    CHECK(profile.failed());
    CHECK(profile_source::evaluations == 1);
    runtime.stop(profile);
    profile.finish();
}

TEST_CASE("runtime graph profiling rolls back a cycle after node evaluation throws",
          "[evaluation][profile][feature:profiling]") {
    const auto throwing_node = bg::node<throwing_profile_node>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)throwing_node();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{};
    throwing_profile_node::stops = 0;

    runtime.start(time(1), profile);
    CHECK_THROWS_AS(runtime.evaluate(time(2), profile), std::runtime_error);
    CHECK(profile.failed());
    CHECK(throwing_profile_node::stops == 1);

    profile.finish();
    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    CHECK_FALSE(reader.read().has_value());
}

TEST_CASE("runtime graph rejects a profile that was not used at start",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto profile = bg::EvaluationProfile{};
    profile_source::evaluations = 0;

    runtime.start(time(1));
    CHECK_THROWS_AS(runtime.evaluate(time(2), profile), std::logic_error);
    CHECK(profile_source::evaluations == 0);
    runtime.stop();
}
