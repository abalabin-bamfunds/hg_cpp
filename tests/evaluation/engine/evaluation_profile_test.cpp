#include <bg/evaluation_profile.hpp>
#include <bg/global_state.hpp>
#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/nested.hpp>
#include <bg/reduce.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace bg;

namespace {

using namespace bg::literals;

struct profile_source {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const {
        output.set(++evaluations);
    }

    inline static i64 evaluations{};
};

struct throwing_profile_node {
    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const { throw std::runtime_error{"profile test evaluation failure"}; }
    void stop() const noexcept { ++stops; }

    inline static std::size_t stops{};
};

struct profiling_counter_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;
    State<i64> value;

    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate(EvaluationContext *context) const {
        output.set(++*value);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

const auto profile_increment = bg::node([](i64 input) { return input + 1; });

EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("runtime graph profiling records node and cycle checkpoints",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{256};
    profile_source::evaluations = 0;

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.stop(profile);
    profile.finish();

    CHECK_FALSE(profile.failed());
    CHECK(profile.recorded_bytes() > 16);

    auto reader = EvaluationProfileReader{profile.take_recording()};
    CHECK((reader.clock() == EvaluationProfileClock::timestamp_counter ||
           reader.clock() == EvaluationProfileClock::steady_clock_nanoseconds));

    auto graph_start_count = std::size_t{};
    auto graph_stop_count = std::size_t{};
    auto graph_entry_count = std::size_t{};
    auto graph_exit_count = std::size_t{};
    auto root_graph_id = GraphInstanceId{};
    auto cycle_start_count = std::size_t{};
    auto cycle_end_count = std::size_t{};
    auto node_count = std::size_t{};
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<EvaluationProfileGraph>(&*record)) {
            ++graph_start_count;
            root_graph_id = graph->graph_id;
            CHECK(graph->graph_id.value != 0);
            CHECK(graph->compiled_graph == &compiled);
            CHECK(graph->parent_graph_id.value == 0);
            CHECK(graph->owner_node_index == UINT32_MAX);
        } else if (std::holds_alternative<EvaluationProfileGraphStop>(*record)) {
            ++graph_stop_count;
        } else if (std::holds_alternative<EvaluationProfileGraphEntry>(*record)) {
            ++graph_entry_count;
        } else if (std::holds_alternative<EvaluationProfileGraphExit>(*record)) {
            ++graph_exit_count;
        } else if (const auto *cycle = std::get_if<EvaluationProfileCycle>(&*record)) {
            if (cycle->boundary == EvaluationProfileCycle::Boundary::start) {
                ++cycle_start_count;
                CHECK(cycle->graph_id == root_graph_id);
                CHECK(cycle->evaluation_time == time(2));
            } else {
                ++cycle_end_count;
                CHECK(cycle->graph_id == root_graph_id);
                CHECK(cycle->evaluation_time == time(2));
            }
        } else if (std::holds_alternative<EvaluationProfileNode>(*record)) {
            ++node_count;
        }
    }

    CHECK(graph_start_count == 1);
    CHECK(graph_stop_count == 1);
    CHECK(graph_entry_count == 0);
    CHECK(graph_exit_count == 0);
    CHECK(cycle_start_count == 1);
    CHECK(cycle_end_count == 1);
    CHECK(node_count == 1);
    CHECK(profile_source::evaluations == 1);
}

TEST_CASE("runtime graph profiling records nested graph ownership",
          "[evaluation][profile][feature:profiling][feature:nested-graph]") {
    const auto inner =
        bg::graph("name"_n = "profile_inner", [](bg::wp<TS<i64>> input) { return profile_increment(input); });
    const auto outer = bg::graph("name"_n = "profile_outer", [&] {
        return bg::nested(inner, bg::operators::const_(true), bg::operators::const_(3));
    });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)outer();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{256};

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto root_graph_id = GraphInstanceId{};
    auto nested_graph_count = std::size_t{};
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<EvaluationProfileGraph>(&*record)) {
            if (graph->parent_graph_id.value == 0) {
                root_graph_id = graph->graph_id;
                CHECK(graph->owner_node_index == UINT32_MAX);
                continue;
            }

            ++nested_graph_count;
            CHECK(graph->parent_graph_id == root_graph_id);
            CHECK(graph->owner_node_index != UINT32_MAX);
        }
    }

    CHECK(root_graph_id.value != 0);
    CHECK(nested_graph_count == 1);
}

TEST_CASE("runtime graph profiling balances graph entries while resuming pending mesh dependencies",
          "[evaluation][profile][feature:profiling][feature:mesh][feature:nested-graph]") {
    auto profile_mesh_events = std::vector<int>{};
    const auto values = bg::node([](In<TS<i64>>, Out<TSD<i64, TS<i64>>> output) {
        output[2].set(20);
        output[1].set(10);
    });

    const auto dependencies = bg::node([](In<TS<i64>>, Out<TSD<i64, TS<i64>>> output) { output[2].set(1); });

    const auto record_dependency_key =
        bg::node([&profile_mesh_events](In<TS<i64, TimeSeriesGuarantee::const_>> key, In<TS<i64>> dependency_key,
                                        Out<TS<i64>> output) {
            profile_mesh_events.push_back(static_cast<int>(*key * 10 + 1));
            output.set(*dependency_key);
        });

    const auto add_dependency =
        bg::node([&profile_mesh_events](In<TS<i64, TimeSeriesGuarantee::const_>> key, In<TS<i64>> value,
                                        In<TS<i64>, bg::optional> dependency, Out<TS<i64>> output) {
            profile_mesh_events.push_back(static_cast<int>(*key * 10 + 2));
            output.set(*key + *value * 0 + (dependency.valid() ? *dependency : 0));
        });

    const auto source = bg::node<profile_source>();

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto input = source();
        (void)bg::mesh(
            [&](auto key, auto item, auto dependency_key) {
                const auto requested_key = record_dependency_key(key, dependency_key);
                return add_dependency(key, item, bg::mesh()[requested_key]);
            },
            values(input), dependencies(input), "__key_arg__"_n = 0);
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};

    SimulationEvaluationEngine{}.run(runtime, time(1), profile);
    profile.finish();
    CHECK(profile_mesh_events == std::vector{21, 12, 22});

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto active_graphs = std::vector<GraphInstanceId>{};
    auto parent_by_graph = std::unordered_map<std::uint64_t, std::uint64_t>{};
    auto entries_in_cycle = std::unordered_map<std::uint64_t, std::size_t>{};
    auto exits_in_cycle = std::unordered_map<std::uint64_t, std::size_t>{};
    auto entries = std::size_t{};
    auto exits = std::size_t{};
    auto node_records = std::size_t{};
    auto nested_graph_reentered_in_cycle = false;
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<EvaluationProfileGraph>(&*record)) {
            parent_by_graph[graph->graph_id.value] = graph->parent_graph_id.value;
        } else if (const auto *cycle = std::get_if<EvaluationProfileCycle>(&*record)) {
            if (cycle->boundary == EvaluationProfileCycle::Boundary::start) {
                REQUIRE(active_graphs.empty());
                active_graphs.push_back(cycle->graph_id);
                entries_in_cycle.clear();
                exits_in_cycle.clear();
            } else {
                REQUIRE_FALSE(active_graphs.empty());
                CHECK(active_graphs.back() == cycle->graph_id);
                active_graphs.pop_back();
                for (const auto &[graph_id, entry_count] : entries_in_cycle) {
                    if (entry_count > 1 && entry_count == exits_in_cycle[graph_id]) {
                        nested_graph_reentered_in_cycle = true;
                    }
                }
            }
        } else if (const auto *entry = std::get_if<EvaluationProfileGraphEntry>(&*record)) {
            active_graphs.push_back(entry->graph_id);
            if (parent_by_graph[entry->graph_id.value] != 0) {
                ++entries_in_cycle[entry->graph_id.value];
            }
            ++entries;
        } else if (const auto *exit = std::get_if<EvaluationProfileGraphExit>(&*record)) {
            REQUIRE_FALSE(active_graphs.empty());
            CHECK(active_graphs.back() == exit->graph_id);
            active_graphs.pop_back();
            if (parent_by_graph[exit->graph_id.value] != 0) {
                ++exits_in_cycle[exit->graph_id.value];
            }
            ++exits;
        } else if (const auto *node = std::get_if<EvaluationProfileNode>(&*record)) {
            REQUIRE_FALSE(active_graphs.empty());
            CHECK(node->graph_id == active_graphs.back());
            ++node_records;
        }
    }

    CHECK(active_graphs.empty());
    CHECK(entries == exits);
    CHECK(node_records > 0);
    CHECK(nested_graph_reentered_in_cycle);
}

TEST_CASE("map profiling stores a key label until the reader consumes it",
          "[evaluation][profile][feature:profiling][feature:map]") {
    const auto keys = bg::node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        if (*value == 1) {
            output[41].set(*value);
        } else {
            output.clear();
        }
    });
    const auto source = bg::node<profiling_counter_source>();

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto input = source();
        (void)bg::map([](auto, auto item) { return profile_increment(item); }, keys(input),
                      "__key_arg__"_n = 0);
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.evaluate(time(3), profile);
    runtime.evaluate(time(4), profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto labelled_graphs = std::size_t{};
    while (const auto record = reader.read()) {
        const auto *graph = std::get_if<EvaluationProfileGraph>(&*record);
        if (graph == nullptr || !graph->label) {
            continue;
        }

        ++labelled_graphs;
        CHECK(graph->parent_graph_id.value != 0);
        CHECK(*bg::scalar_cast<i64>(*graph->label) == 41);
    }
    CHECK(labelled_graphs == 1);
}

TEST_CASE("compact reduce profiles pair-slot reuse as stop then start of the same graph ID",
          "[evaluation][profile][feature:profiling][feature:reduce]") {
    const auto values = bg::node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) {
        if (*input == 1) {
            output[1].set(10);
            output[2].set(20);
            output[3].set(30);
        } else if (*input == 2) {
            output.erase(2);
            output[4].set(40);
        }
    });
    const auto add =
        bg::node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) { output.set(*left + *right); });
    const auto reducer = bg::graph("name"_n = "profile_reduce_pair",
                                   [add](bg::wp<TS<i64>> left, bg::wp<TS<i64>> right) { return add(left, right); });
    const auto sink = bg::node([](In<TS<i64>>) {});
    const auto source = bg::node<profiling_counter_source>();

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto input = source();
        sink(bg::reduce(reducer, values(input)));
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.evaluate(time(3), profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto root_graph_id = GraphInstanceId{};
    auto graph_is_active = std::unordered_map<std::uint64_t, bool>{};
    auto graph_start_count = std::unordered_map<std::uint64_t, std::size_t>{};
    auto slot_reused = false;
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<EvaluationProfileGraph>(&*record)) {
            if (graph->parent_graph_id.value == 0) {
                root_graph_id = graph->graph_id;
                continue;
            }
            if (graph->parent_graph_id != root_graph_id) {
                continue;
            }

            CHECK_FALSE(graph_is_active[graph->graph_id.value]);
            slot_reused |= graph_start_count[graph->graph_id.value] != 0;
            graph_is_active[graph->graph_id.value] = true;
            ++graph_start_count[graph->graph_id.value];
        } else if (const auto *stop = std::get_if<EvaluationProfileGraphStop>(&*record)) {
            if (graph_is_active.contains(stop->graph_id.value)) {
                CHECK(graph_is_active[stop->graph_id.value]);
                graph_is_active[stop->graph_id.value] = false;
            }
        }
    }

    CHECK(root_graph_id.value != 0);
    CHECK(slot_reused);
    CHECK(std::ranges::none_of(graph_is_active, [](const auto &entry) { return entry.second; }));
}

struct profile_start_only {
    void start(EvaluationContext *context) const {
        ++starts;
        context->schedule_now();
    }

    inline static std::size_t starts{};
};

TEST_CASE("runtime graph profiling records scheduled nodes without evaluation functions",
          "[evaluation][profile][feature:profiling]") {
    const auto scheduled = bg::node<profile_start_only>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)scheduled();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    profile_start_only::starts = 0;

    runtime.start(time(1), profile);
    runtime.evaluate(time(2), profile);
    runtime.stop(profile);
    profile.finish();

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto node_count = std::size_t{};
    while (const auto record = reader.read()) {
        node_count += static_cast<std::size_t>(std::holds_alternative<EvaluationProfileNode>(*record));
    }

    CHECK(node_count == 1);
    CHECK(profile_start_only::starts == 1);
}

TEST_CASE("simulation evaluation engine records a profile",
          "[evaluation][profile][feature:profiling][feature:simulation]") {
    const auto source = bg::node<profile_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    profile_source::evaluations = 0;

    SimulationEvaluationEngine{}.run(runtime, time(1), profile);
    profile.finish();

    CHECK_FALSE(profile.failed());
    CHECK(profile_source::evaluations == 1);

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto cycle_start_count = std::size_t{};
    auto cycle_end_count = std::size_t{};
    auto graph_stop_count = std::size_t{};
    while (const auto record = reader.read()) {
        if (const auto *cycle = std::get_if<EvaluationProfileCycle>(&*record)) {
            cycle_start_count += cycle->boundary == EvaluationProfileCycle::Boundary::start;
            cycle_end_count += cycle->boundary == EvaluationProfileCycle::Boundary::end;
        } else if (std::holds_alternative<EvaluationProfileGraphStop>(*record)) {
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
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{64, bg::allocator{memory}};
    profile_source::evaluations = 0;

    runtime.start(time(1), profile);
    auto reader = EvaluationProfileReader{profile.take_recording()};
    CHECK_FALSE(reader.read().has_value());

    runtime.evaluate(time(2), profile);
    CHECK(memory.allocations() > 1);

    const auto allocated_bytes_before_drain = memory.allocated_bytes();
    auto record_count = std::size_t{};
    while (reader.read()) {
        ++record_count;
    }
    CHECK(record_count == 4);
    CHECK(memory.allocated_bytes() < allocated_bytes_before_drain);

    runtime.stop(profile);
    profile.finish();
    auto graph_stop_seen = false;
    while (const auto record = reader.read()) {
        graph_stop_seen |= std::holds_alternative<EvaluationProfileGraphStop>(*record);
    }
    CHECK(graph_stop_seen);
}

TEST_CASE("profile transfer before the first cycle preserves pending label cleanup",
          "[evaluation][profile][feature:profiling]") {
    const auto throwing_node = bg::node<throwing_profile_node>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)throwing_node();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    auto label = ScalarValue{std::string_view{"pending graph label"}};
    const auto label_view = label.view();

    runtime.start(time(1), profile, Scalar<>::from(label_view));
    auto reader = EvaluationProfileReader{profile.take_recording()};
    CHECK_FALSE(reader.read().has_value());

    CHECK_THROWS_AS(runtime.evaluate(time(2), profile), std::runtime_error);
    CHECK(profile.failed());
    CHECK_FALSE(reader.read().has_value());
    profile.finish();
}

TEST_CASE("profile transfer during an open cycle preserves pending label cleanup",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto profile = EvaluationProfile{};
    auto label = ScalarValue{std::string_view{"pending graph label"}};
    const auto label_view = label.view();
    const auto graph_id = GraphInstanceId{1};

    profile.begin_cycle(graph_id, time(1), time(1));
    profile.start_graph(GraphInstanceId{2}, compiled, graph_id, 0, Scalar<>::from(label_view));
    auto reader = EvaluationProfileReader{profile.take_recording()};
    CHECK_FALSE(reader.read().has_value());

    profile.abort_cycle();
    CHECK_FALSE(reader.read().has_value());
    profile.finish();
}

TEST_CASE("profile cycle end stores its exact wall time without repeating evaluation time",
          "[evaluation][profile][feature:profiling]") {
    auto profile = EvaluationProfile{};
    const auto graph_id = GraphInstanceId{1};
    const auto start_wall_time = time(100);
    const auto end_wall_time = time(135);

    profile.begin_cycle(graph_id, time(1), start_wall_time);
    profile.record_node(0);
    profile.end_cycle(end_wall_time);
    profile.finish();

    CHECK(profile.recorded_bytes() == 100);
    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto cycle_starts = std::size_t{};
    auto cycle_ends = std::size_t{};
    while (const auto record = reader.read()) {
        const auto *cycle = std::get_if<EvaluationProfileCycle>(&*record);
        if (cycle == nullptr) {
            continue;
        }

        CHECK(cycle->evaluation_time == time(1));
        if (cycle->boundary == EvaluationProfileCycle::Boundary::start) {
            CHECK(cycle->wall_clock_time == start_wall_time);
            ++cycle_starts;
        } else {
            CHECK(cycle->wall_clock_time == end_wall_time);
            ++cycle_ends;
        }
    }
    CHECK(cycle_starts == 1);
    CHECK(cycle_ends == 1);
}

TEST_CASE("profile reader rejects node checkpoints outside an outer cycle",
          "[evaluation][profile][feature:profiling]") {
    auto profile = EvaluationProfile{};
    profile.record_node(0);
    profile.finish();

    auto reader = EvaluationProfileReader{profile.take_recording()};
    CHECK_THROWS_AS(reader.read(), std::runtime_error);
}

TEST_CASE("runtime graph profiling records each graph instance against its compiled graph",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto first = RuntimeGraph{compiled, global_state};
    auto second = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    profile_source::evaluations = 0;

    first.start(time(1), profile);
    second.start(time(1), profile);
    first.evaluate(time(2), profile);
    second.evaluate(time(3), profile);
    first.stop(profile);
    second.stop(profile);
    profile.finish();

    auto reader = EvaluationProfileReader{profile.take_recording()};
    auto graph_start_count = std::size_t{};
    auto first_graph_id = GraphInstanceId{};
    auto second_graph_id = GraphInstanceId{};
    auto cycle_start_graph_ids = std::vector<GraphInstanceId>{};
    while (const auto record = reader.read()) {
        if (const auto *graph = std::get_if<EvaluationProfileGraph>(&*record)) {
            ++graph_start_count;
            CHECK(graph->compiled_graph == &compiled);
            if (first_graph_id.value == 0) {
                first_graph_id = graph->graph_id;
            } else {
                second_graph_id = graph->graph_id;
            }
        } else if (const auto *cycle = std::get_if<EvaluationProfileCycle>(&*record);
                   cycle != nullptr && cycle->boundary == EvaluationProfileCycle::Boundary::start) {
            cycle_start_graph_ids.push_back(cycle->graph_id);
        }
    }

    CHECK(graph_start_count == 2);
    CHECK(first_graph_id.value != 0);
    CHECK(second_graph_id.value != 0);
    CHECK(first_graph_id != second_graph_id);
    CHECK(cycle_start_graph_ids == std::vector{first_graph_id, second_graph_id});
}

TEST_CASE("runtime graph profiling falls back to plain evaluation after recording allocation failure",
          "[evaluation][profile][feature:profiling]") {
    auto memory = counting_memory_resource{};
    const auto source = bg::node<profile_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{32, bg::allocator{memory}};
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
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{32, bg::allocator{memory}};
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
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)throwing_node();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    throwing_profile_node::stops = 0;

    runtime.start(time(1), profile);
    CHECK_THROWS_AS(runtime.evaluate(time(2), profile), std::runtime_error);
    CHECK(profile.failed());
    CHECK(throwing_profile_node::stops == 1);

    profile.finish();
    auto reader = EvaluationProfileReader{profile.take_recording()};
    CHECK_FALSE(reader.read().has_value());
}

TEST_CASE("runtime graph rejects a profile that was not used at start",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto profile = EvaluationProfile{};
    profile_source::evaluations = 0;

    runtime.start(time(1));
    CHECK_THROWS_AS(runtime.evaluate(time(2), profile), std::logic_error);
    CHECK(profile_source::evaluations == 0);
    runtime.stop();
}

struct profile_stop_context_source {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const { output.set(1); }
    void stop(EvaluationContext *context) const noexcept { stopped_profile = context->evaluation_profile(); }

    inline static EvaluationProfile *stopped_profile{};
};

TEST_CASE("runtime graph keeps the profile selected at start",
          "[evaluation][profile][feature:profiling]") {
    const auto source = bg::node<profile_stop_context_source>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)source();
    }
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    auto expected_profile = EvaluationProfile{};
    auto other_profile = EvaluationProfile{};
    profile_stop_context_source::stopped_profile = nullptr;

    runtime.start(time(1), expected_profile);
    CHECK_THROWS_AS(runtime.evaluate(time(2), other_profile), std::logic_error);
    runtime.evaluate(time(2), expected_profile);
    runtime.stop(other_profile);

    CHECK(profile_stop_context_source::stopped_profile == &expected_profile);
    expected_profile.finish();
    other_profile.finish();

    auto expected_reader = EvaluationProfileReader{expected_profile.take_recording()};
    auto expected_stop_seen = false;
    while (const auto record = expected_reader.read()) {
        expected_stop_seen |= std::holds_alternative<EvaluationProfileGraphStop>(*record);
    }
    CHECK(expected_stop_seen);

    auto other_reader = EvaluationProfileReader{other_profile.take_recording()};
    CHECK_FALSE(other_reader.read().has_value());
}
