#include <bg/graph.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map/details/map_node.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../utils/counting_memory_resource.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace {

struct AddKey {
    In<TS<i64>> value;
    In<TS<std::int64_t>> key;
    Out<TS<i64>> output;

    inline static std::array<std::size_t, 3> evaluations{};

    void evaluate() const {
        ++evaluations.at(static_cast<std::size_t>(*key));
        output.set(*value + static_cast<int>(*key));
    }
};

const auto add_key = node<AddKey>();

const auto add_available = node([](In<TS<i64>, optional> left, In<TS<i64>, optional> right, Out<TS<i64>> output) {
    output.set((left.valid() ? *left : 0) + (right.valid() ? *right : 0));
});

const auto add_values =
    node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) { output.set(*left + *right); });

struct DelayedValue {
    In<TS<i64>, passive> input;
    Out<TS<i64>> output;

    static constexpr bool uses_timed_scheduling = true;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const { output.set(*input); }
};

const auto delayed_value = node<DelayedValue>();

struct VariableDelay {
    In<TS<i64>, passive> input;
    Out<TS<i64>> output;

    static constexpr bool uses_timed_scheduling = true;
    inline static std::vector<int> evaluations;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{*input});
    }

    void evaluate() const {
        evaluations.push_back(*input);
        output.set(*input);
    }
};

const auto variable_delay = node<VariableDelay>();

struct MapSink {
    In<TS<i64>> input;

    inline static std::vector<int> values;

    void evaluate() const { values.push_back(*input); }
};

const auto map_sink = node<MapSink>();

struct ControlledKeys {
    Out<TSS<std::int64_t>> output;

    inline static bool present{};

    void evaluate() const {
        if (present) {
            output.insert(1);
            output.insert(2);
        } else {
            output.erase(1);
        }
    }
};

const auto controlled_keys = node<ControlledKeys>();

EvaluationTime time(std::int64_t microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

wp<TSD<std::int64_t, REF<TS<i64>>>> wire_map(wp<TSD<std::int64_t, REF<TS<i64>>>> values) {
    auto inner = std::make_shared<GraphWiring>();
    auto value_node = std::size_t{};
    auto key_node = std::size_t{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto value = details::wire_nested_input(TS<i64>::type());

        const auto key = details::wire_nested_constant_input(TS<std::int64_t, TimeSeriesGuarantee::const_>::type());

        const auto result = add_key(wp<REF<TS<i64>>>{value}, wp<TS<std::int64_t, TimeSeriesGuarantee::const_>>{key});
        const auto output = details::wire_nested_output(result, TS<i64>::type());
        const auto concrete_output = inner->complete_pending_wiring(output);
        value_node = inner->output_node_index(value);
        key_node = inner->output_node_index(key);
        output_node = inner->output_node_index(concrete_output);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_map"},
        .parameters = {{.name = util::symbol{"values"},
                        .type = &values.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = &TSD<std::int64_t, REF<TS<i64>>>::type(),
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner),
         .inputs = {{.kind = details::MapInputKind::mapped, .parameter_index = 0, .input_node = value_node}},
         .key_input_node = key_node,
         .output_node = output_node},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    return wp<TSD<std::int64_t, REF<TS<i64>>>>{*owned(values)};
}

wp<TSD<std::int64_t, REF<TS<i64>>>> wire_union_map(wp<TSD<std::int64_t, REF<TS<i64>>>> left,
                                                   wp<TSD<std::int64_t, REF<TS<i64>>>> right) {
    auto inner = std::make_shared<GraphWiring>();
    auto left_node = std::size_t{};
    auto right_node = std::size_t{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto left_input = details::wire_nested_input(TS<i64>::type());
        const auto right_input = details::wire_nested_input(TS<i64>::type());

        const auto result = add_available(wp<REF<TS<i64>>>{left_input}, wp<REF<TS<i64>>>{right_input});
        const auto output = details::wire_nested_output(result, TS<i64>::type());
        const auto concrete_output = inner->complete_pending_wiring(output);
        left_node = inner->output_node_index(left_input);
        right_node = inner->output_node_index(right_input);
        output_node = inner->output_node_index(concrete_output);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_union_map"},
        .parameters = {{.name = util::symbol{"left"},
                        .type = &left.type().target_type(),
                        .activity = active,
                        .validity = required},
                       {.name = util::symbol{"right"},
                        .type = &right.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = &TSD<std::int64_t, REF<TS<i64>>>::type(),
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner),
         .inputs = {{.kind = details::MapInputKind::mapped, .parameter_index = 0, .input_node = left_node},
                    {.kind = details::MapInputKind::mapped, .parameter_index = 1, .input_node = right_node}},
         .output_node = output_node},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    return wp<TSD<std::int64_t, REF<TS<i64>>>>{*owned(left, right)};
}

template <typename KeySet>
wp<TSD<std::int64_t, REF<TS<std::int64_t, TimeSeriesGuarantee::const_>>>> wire_source_map(wp<KeySet> keys) {
    auto inner = std::make_shared<GraphWiring>();
    auto key_node = std::size_t{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto key = details::wire_nested_constant_input(TS<std::int64_t, TimeSeriesGuarantee::const_>::type());
        const auto output = details::wire_nested_output(key, TS<std::int64_t, TimeSeriesGuarantee::const_>::type());
        const auto concrete_output = inner->complete_pending_wiring(output);
        key_node = inner->output_node_index(key);
        output_node = inner->output_node_index(concrete_output);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_source_map"},
        .parameters = {{.name = util::symbol{"keys"},
                        .type = &keys.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = &TSD<std::int64_t, REF<TS<std::int64_t, TimeSeriesGuarantee::const_>>>::type(),
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner), .key_input_node = key_node, .output_node = output_node, .key_set_parameter = 0},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    return wp<TSD<std::int64_t, REF<TS<std::int64_t, TimeSeriesGuarantee::const_>>>>{*owned(keys)};
}

void wire_source_sink_map(wp<TSS<std::int64_t>> keys) {
    auto inner = std::make_shared<GraphWiring>();
    auto key_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto key = details::wire_nested_constant_input(TS<std::int64_t, TimeSeriesGuarantee::const_>::type());
        (void)inner->complete_pending_wiring();
        key_node = inner->output_node_index(key);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_source_sink_map"},
        .parameters = {{.name = util::symbol{"keys"},
                        .type = &keys.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = nullptr,
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner), .key_input_node = key_node, .key_set_parameter = 0}, signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    const auto output = owned(keys);
    REQUIRE_FALSE(output);
}

wp<TSD<std::int64_t, REF<TS<i64>>>> wire_delayed_map(wp<TSD<std::int64_t, REF<TS<i64>>>> values) {
    auto inner = std::make_shared<GraphWiring>();
    auto value_node = std::size_t{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto value = details::wire_nested_input(TS<i64>::type());
        const auto result = delayed_value(wp<REF<TS<i64>>>{value});
        const auto output = details::wire_nested_output(result, TS<i64>::type());
        const auto concrete_output = inner->complete_pending_wiring(output);
        value_node = inner->output_node_index(value);
        output_node = inner->output_node_index(concrete_output);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_delayed_map"},
        .parameters = {{.name = util::symbol{"values"},
                        .type = &values.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = &TSD<std::int64_t, REF<TS<i64>>>::type(),
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner),
         .inputs = {{.kind = details::MapInputKind::mapped, .parameter_index = 0, .input_node = value_node}},
         .output_node = output_node},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    return wp<TSD<std::int64_t, REF<TS<i64>>>>{*owned(values)};
}

wp<TSD<std::int64_t, REF<TS<i64>>>> wire_variable_delayed_map(wp<TSD<std::int64_t, REF<TS<i64>>>> values) {
    auto inner = std::make_shared<GraphWiring>();
    auto value_node = std::size_t{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto value = details::wire_nested_input(TS<i64>::type());
        const auto result = variable_delay(wp<REF<TS<i64>>>{value});
        const auto output = details::wire_nested_output(result, TS<i64>::type());
        const auto concrete_output = inner->complete_pending_wiring(output);
        value_node = inner->output_node_index(value);
        output_node = inner->output_node_index(concrete_output);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_variable_delayed_map"},
        .parameters = {{.name = util::symbol{"values"},
                        .type = &values.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = &TSD<std::int64_t, REF<TS<i64>>>::type(),
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner),
         .inputs = {{.kind = details::MapInputKind::mapped, .parameter_index = 0, .input_node = value_node}},
         .output_node = output_node},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    return wp<TSD<std::int64_t, REF<TS<i64>>>>{*owned(values)};
}

void wire_sink_map(wp<TSD<std::int64_t, REF<TS<i64>>>> values) {
    auto inner = std::make_shared<GraphWiring>();
    auto value_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto value = details::wire_nested_input(TS<i64>::type());
        map_sink(wp<REF<TS<i64>>>{value});
        (void)inner->complete_pending_wiring();
        value_node = inner->output_node_index(value);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_sink_map"},
        .parameters = {{.name = util::symbol{"values"},
                        .type = &values.type().target_type(),
                        .activity = active,
                        .validity = required}},
        .output = nullptr,
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner),
         .inputs = {{.kind = details::MapInputKind::mapped, .parameter_index = 0, .input_node = value_node}}},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    const auto output = owned(values);
    REQUIRE_FALSE(output);
}

wp<TSD<std::int64_t, REF<TS<i64>>>> wire_pass_through_map(wp<TSD<std::int64_t, REF<TS<i64>>>> values,
                                                          wp<REF<TS<i64>>> factor) {
    auto inner = std::make_shared<GraphWiring>();
    auto value_node = std::size_t{};
    auto factor_node = std::size_t{};
    auto output_node = std::size_t{};
    {
        auto context = WiringContext{*inner};
        const auto value = details::wire_nested_input(TS<i64>::type());
        const auto factor_input = details::wire_nested_input(TS<i64>::type());
        const auto result = add_values(wp<REF<TS<i64>>>{value}, wp<REF<TS<i64>>>{factor_input});
        const auto output = details::wire_nested_output(result, TS<i64>::type());
        const auto concrete_output = inner->complete_pending_wiring(output);
        value_node = inner->output_node_index(value);
        factor_node = inner->output_node_index(factor_input);
        output_node = inner->output_node_index(concrete_output);
    }
    inner->finalize();

    auto signature = ResolvedSignature{
        .name = util::symbol{"test_pass_through_map"},
        .parameters =
            {{.name = util::symbol{"values"},
              .type = &values.type().target_type(),
              .activity = active,
              .validity = required},
             {.name = util::symbol{"factor"}, .type = &factor.type(), .activity = active, .validity = required}},
        .output = &TSD<std::int64_t, REF<TS<i64>>>::type(),
    };
    auto node_template = details::make_map_node(
        {.wiring = std::move(inner),
         .inputs = {{.kind = details::MapInputKind::mapped, .parameter_index = 0, .input_node = value_node},
                    {.kind = details::MapInputKind::pass_through, .parameter_index = 1, .input_node = factor_node}},
         .output_node = output_node},
        signature);
    auto &wiring = WiringContext::current().wiring();
    const auto &owned = wiring.own_node_template(std::move(node_template));
    return wp<TSD<std::int64_t, REF<TS<i64>>>>{*owned(values, factor)};
}

} // namespace

TEST_CASE("map starts and stops one inner graph repeatedly",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto target = node([](In<TS<i64>> command, Out<TS<i64>> output) { output.set(std::abs(*command)); });
    const auto make_values =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1].copy(value);
            }
        });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto values = make_values(command, target(command));
        return wire_map(values)[std::int64_t{1}];
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, -1, 3, -1, 4});

    CHECK(actual.equals<bg::i64>({2, 3, std::nullopt, 4, std::nullopt, 5}));
}

TEST_CASE("mapped target ticks evaluate only the selected inner graph",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[transition:update]") {
    const auto targets = node([](In<TS<i64>> command, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*command == 0) {
            output[1].set(10);
            output[2].set(20);
        } else {
            output[*command].set(*command * 10);
        }
    });
    const auto rekey =
        node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            output[1].copy(first);
            output[2].copy(second);
        });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto values = targets(command);
        const auto mapped = wire_map(rekey(values[std::int64_t{1}], values[std::int64_t{2}]));
        return mapped[std::int64_t{2}];
    };
    AddKey::evaluations.fill(0);

    const auto actual = testing::test_graph(graph, std::array{0, 2});

    CHECK(actual.equals<bg::i64>({22, 22}));
    CHECK(AddKey::evaluations == std::array<std::size_t, 3>{0, 1, 2});
}

TEST_CASE("map evaluates more inner graphs than the ready bitmap inline capacity",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]") {
    const auto make_values = node([](In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
        for (auto key = std::int64_t{}; key < 100; ++key) {
            output[key].copy(value);
        }
    });
    const auto graph = [&](wp<TS<i64>> value) {
        wire_sink_map(make_values(value.as<REF<TS<i64>>>()));
        return value;
    };
    MapSink::values.clear();

    const auto result = testing::test_graph(graph, std::array{7});

    CHECK(result.equals<bg::i64>({7}));
    CHECK(MapSink::values.size() == 100);
}

TEST_CASE("stopped map inner graphs are destroyed at the start of the next outer cycle",
          "[feature:map][feature:nested-graph][feature:lifecycle][series:tss][shape:nested]"
          "[transition:removal]") {
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        wire_source_sink_map(controlled_keys());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    {
        auto runtime_global_state = bg::GlobalState{allocator{memory}};
        auto runtime = RuntimeGraph{compiled, allocator{memory}, runtime_global_state};
        ControlledKeys::present = true;
        runtime.start(time(1));
        runtime.schedule(0);
        runtime.evaluate(time(1));

        ControlledKeys::present = false;
        runtime.schedule(0);
        runtime.evaluate(time(2));
        const auto stopped_allocations = memory.allocations();

        runtime.evaluate(time(3));

        // The inner runtime graph object and its storage each use one allocation.
        CHECK(stopped_allocations - memory.allocations() == 2);
    }

    CHECK(memory.allocations() == 0);
}

TEST_CASE("map uses the union of mapped dictionary key sets and binds missing values",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto target = node([](In<TS<i64>> value, Out<TS<i64>> output) { output.set(std::abs(*value)); });
    const auto make_values =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            if (*command < 0) {
                output.clear();
            } else {
                output[1].copy(value);
            }
        });
    const auto graph = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        const auto left_values = make_values(left, target(left));
        const auto right_values = make_values(right, target(right));
        return wire_union_map(left_values, right_values)[std::int64_t{1}];
    };

    const auto actual = testing::test_graph(graph, std::array{1, -1, -1, 4}, std::array{-1, 2, -1, 3});

    CHECK(actual.equals<bg::i64>({1, 2, std::nullopt, 7}));
}

TEST_CASE("map uses a TSS key set for a source graph",
          "[feature:map][feature:nested-graph][feature:reference][series:tss][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto make_keys = node([](In<TS<bool>> present, Out<TSS<std::int64_t>> output) {
        if (*present) {
            output.insert(1);
        } else {
            output.erase(1);
        }
    });
    const auto graph = [&](wp<TS<bool>> present) { return wire_source_map(make_keys(present))[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({1, std::nullopt, 1}));
}

TEST_CASE("map uses a selected TSD key set for a source graph",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto make_keys = node([](In<TS<bool>> present, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*present) {
            output[1].set(0);
        } else {
            output.erase(1);
        }
    });
    const auto graph = [&](wp<TS<bool>> present) { return wire_source_map(make_keys(present))[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({1, std::nullopt, 1}));
}

TEST_CASE("a sink map evaluates one inner graph for each live key",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[transition:addition][transition:update][transition:removal]") {
    const auto target = node([](In<TS<i64>> command, Out<TS<i64>> output) { output.set(std::abs(*command)); });
    const auto make_values =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1].copy(value);
            }
        });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto value = target(command);
        wire_sink_map(make_values(command, value));
        return value;
    };
    MapSink::values.clear();

    const auto actual = testing::test_graph(graph, std::array{1, 2, -1, 3});

    CHECK(actual.equals<bg::i64>({1, 2, 1, 3}));
    CHECK(MapSink::values == std::vector{1, 2, 3});
}

TEST_CASE("a pass-through reference replacement updates each running map inner graph",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[binding:reference-to-reference][transition:update]") {
    const auto target = node([](In<TS<i64>> value, int scale, Out<TS<i64>> output) { output.set(*value * scale); });
    const auto choose = node([](In<TS<bool>> condition, In<REF<TS<i64>>> left, In<REF<TS<i64>>> right,
                                Out<REF<TS<i64>>> output) { output.copy(*condition ? left : right); });
    const auto make_values =
        node([](In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output[1].copy(value); });
    const auto graph = [&](wp<TS<i64>> value, wp<TS<bool>> use_left) {
        const auto mapped = make_values(target(value, 1));
        const auto factor = choose(use_left, target(value, 10), target(value, 100));
        return wire_pass_through_map(mapped, factor)[std::int64_t{1}];
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, std::array{true, false, true});

    CHECK(actual.equals<bg::i64>({11, 202, 33}));
}

TEST_CASE("a map inner graph reports timed work through the map owner",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[feature:timed-scheduling]") {
    const auto target = node([](In<TS<i64>> value, Out<TS<i64>> output) { output.set(*value); });
    const auto make_values =
        node([](In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output[1].copy(value); });
    const auto graph = [&](wp<TS<i64>> value) { return wire_delayed_map(make_values(target(value)))[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{5});

    REQUIRE(actual.size() == 3);
    CHECK_FALSE(actual[0]);
    CHECK_FALSE(actual[1]);
    REQUIRE(actual[2]);
    CHECK(actual[2]->value() == 5);
}

TEST_CASE("map schedules the next scheduled time after one inner graph becomes due",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[feature:timed-scheduling]") {
    const auto targets = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(2);
        output[2].set(4);
    });
    const auto rekey =
        node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            output[1].copy(first);
            output[2].copy(second);
        });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto values = targets(command);
        const auto mapped = wire_variable_delayed_map(rekey(values[std::int64_t{1}], values[std::int64_t{2}]));
        return mapped[std::int64_t{2}];
    };
    VariableDelay::evaluations.clear();

    const auto actual = testing::test_graph(graph, std::array{0});

    CHECK(VariableDelay::evaluations == std::vector{2, 4});
    REQUIRE(actual[actual.size() - 1]);
    CHECK(actual[actual.size() - 1]->value() == 4);
}

TEST_CASE("a map consumes and produces TSD references across two nested graph boundaries",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:doubly-nested]"
          "[binding:reference-to-reference][transition:stop][transition:restart]") {
    const auto make_values =
        node([](In<REF<TS<i64>>> value, Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output[1].copy(value); });
    const auto build_dictionary =
        graph("name"_n = "build_dictionary", [&](wp<TS<i64>> value) -> wp<TSD<std::int64_t, REF<TS<i64>>>> {
            return make_values(value.as<REF<TS<i64>>>());
        });
    const auto apply_map = graph("name"_n = "apply_map",
                                 [](wp<TSD<std::int64_t, REF<TS<i64>>>> values) -> wp<TSD<std::int64_t, REF<TS<i64>>>> {
                                     return wire_map(values);
                                 });
    const auto outer = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        const auto first = nested(build_dictionary, run, value);
        const auto second = nested(apply_map, run, first);
        return second[std::int64_t{1}];
    };

    const auto actual =
        testing::test_graph(outer, std::array{1, 2, 3, 4, 5}, std::array{true, true, false, true, true});

    CHECK(actual.equals<bg::i64>({2, 3, std::nullopt, 5, 6}));
}
