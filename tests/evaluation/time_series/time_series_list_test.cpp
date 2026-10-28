#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>
#include <bg/switch.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace bg;
using namespace literals;

TEST_CASE("a node can select an item from a free fixed time series list",
          "[evaluation][tsl][feature:getitem][series:fixed-tsl][free][representation:free][scalar:integer]"
          "[declaration:concrete][binding:direct]") {
    const auto select = node([](In<TSL<TS<i64>, 2>> input) { return *input[1]; });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return select(tsl(first, second));
    };
    const auto result = bg::run_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({10, 20}));
}

TEST_CASE("a fixed time series list publishes changed items",
          "[evaluation][tsl][series:fixed-tsl][feature:collection-change][transition:update]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    struct observation {
        i64 first_value;
        i64 third_value;
        bool first_updated;
        bool middle_updated;
        bool third_updated;

        bool operator==(const observation &) const = default;
    };

    auto evaluation_count = 0;

    const auto copy = node([](In<TSL<TS<SCALAR>, 3>> input, Out<TSL<TS<SCALAR>, 3>> output) {
        for (auto item = std::size_t{}; item < 3; ++item) {
            if (input[item].valid()) {
                output[item].set_value(input[item].value_view());
            }
        }
    });

    const auto inspect = node([&](In<TSL<TS<i64>, 3>> input) {
        ++evaluation_count;
        return observation{*input[0], *input[2], input.item_updated(0), input.item_updated(1), input.item_updated(2)};
    });

    const auto wiring = graph("name"_n = "literal_list_input",
                              [&](wp<TSL<TS<i64>, 3>> input) { return inspect(copy(input)); });

    const auto actual = bg::run_graph(wiring, {{{0, 10}, {2, 30}}});

    CHECK(actual.equals<observation>({observation{10, 30, true, false, true}}));
    CHECK(evaluation_count == 1);
}

TEST_CASE("a free fixed time series list coalesces source changes",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][feature:collection-change]"
          "[transition:update][scalar:integer][declaration:concrete][binding:direct]") {
    struct observation {
        i64 sum;
        bool first_updated;
        bool second_updated;

        bool operator==(const observation &) const = default;
    };

    auto evaluation_count = 0;

    const auto sum = node([&](In<TSL<TS<i64>, 2>> input) {
        ++evaluation_count;
        return observation{*input[0] + *input[1], input.item_updated(0), input.item_updated(1)};
    });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(tsl(first, second));
    };

    const auto result = bg::run_graph(graph, {1, 2}, {10, literal::SKIP});

    CHECK(result.equals<observation>({observation{11, true, true}, observation{12, true, false}}));
    CHECK(evaluation_count == 2);
}

TEST_CASE("a free fixed list reports item updates and partial invalidity",
          "[feature:collection-change][series:fixed-tsl][representation:free][shape:single-node]"
          "[transition:update][transition:invalidation][transition:revalidation][validity:optional]"
          "[scalar:integer][binding:direct]") {
    struct Observation {
        bool list_valid;
        bool first_valid;
        bool second_valid;
        i64 first;
        i64 second;
        bool first_updated;
        bool second_updated;

        bool operator==(const Observation &) const = default;
    };

    const auto observe = node([](In<TSL<TS<i64>, 2>, optional> input) {
        const auto first = input[0];
        const auto second = input[1];
        return Observation{input.valid(), first.valid(), second.valid(), first.valid() ? *first : -1,
                           second.valid() ? *second : -1, first.modified(), second.modified()};
    });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) { return observe(tsl(first, second)); };

    const auto actual = bg::run_graph(graph,
                                      {10, 20, literal::INVALID, 30},
                                      {1, literal::SKIP, 2, literal::SKIP});

    CHECK(actual.equals<Observation>({Observation{true, true, true, 10, 1, true, true},
                                      Observation{true, true, true, 20, 1, true, false},
                                      Observation{true, false, true, -1, 2, true, true},
                                      Observation{true, true, true, 30, 2, true, false}}));
}

TEST_CASE("a free fixed list can be observed through a reference",
          "[feature:reference][series:fixed-tsl][series:reference][representation:free]"
          "[binding:value-to-reference][transition:update]") {
    const auto observe = node([](In<REF<TSL<TS<i64>, 2>>> input) { return input.valid(); });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second) { return observe(tsl(first, second)); };

    const auto actual = bg::run_graph(tested, {1, 2}, {10, 20});

    CHECK(actual.equals({true, std::nullopt}));
}

TEST_CASE("a free fixed time series list follows the receiving item runtime_type",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][reference]"
          "[feature:collection-change][scalar:integer][declaration:concrete][binding:direct]") {
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(tsl(reference(first), second));
    };
    const auto result = bg::run_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({11, 22}));
}

TEST_CASE("a free fixed time series list preserves compound child rebinding deltas",
          "[evaluation][tsl][series:fixed-tsl][series:tsd][series:reference][feature:collection-change]"
          "[transition:update][transition:removal][binding:reference-to-value][representation:free]") {
    const auto select = node([](bool use_second, In<REF<TSD<i64, TS<i64>>>, passive> first,
                                In<REF<TSD<i64, TS<i64>>>, passive> second,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(use_second ? second : first);
    });
    const auto graph = [&](wp<TS<bool>> choose, wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        return tsl(select(choose, first, second));
    };

    const auto actual = bg::run_graph(graph, {false, true, false}, {{{1, 10}}}, {{{2, 20}}});

    CHECK(actual.equals({{{0, {{1, 10}}}},
                         {{0, {{1, literal::REMOVE}, {2, 20}}}},
                         {{0, {{2, literal::REMOVE}, {1, 10}}}}}));
}

TEST_CASE("a span-backed free TSL retains its items through materialisation and evaluation",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][feature:wiring-materialisation]"
          "[scalar:integer]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto collection = [&] {
            const auto items = std::vector<WiringPoint>{first, second};
            return tsl(std::span<const WiringPoint>{items});
        }();
        return sum(collection);
    };

    const auto result = bg::run_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({11, 22}));
}

TEST_CASE("an empty typed TSL publishes valid at graph start",
          "[evaluation][tsl][series:fixed-tsl][feature:wiring-materialisation][representation:free]"
          "[transition:start]") {
    const auto observe = node([](In<TSL<TS<i64>, 0>> input) { return input.valid() && input.size() == 0; });

    const auto graph = [&] { return observe(tsl<TS<i64>>()); };

    CHECK(bg::run_graph(graph).equals({true}));
}

TEST_CASE("a fixed time series list schedules subscribers of changed items",
          "[evaluation][tsl][series:fixed-tsl][scheduling][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    auto first_evaluations = 0;
    auto second_evaluations = 0;

    const auto first_sink = node([&](i64) { ++first_evaluations; });

    const auto second_sink = node([&](i64) { ++second_evaluations; });

    const auto first = node([](In<TSL<TS<i64>, 3>> input, Out<TS<i64>> output) {
        if (input.item_updated(0)) {
            output.set(*input[0]);
        }
    });

    const auto second = node([](In<TSL<TS<i64>, 3>> input, Out<TS<i64>> output) {
        if (input.item_updated(1)) {
            output.set(*input[1]);
        }
    });

    const auto wiring = graph("name"_n = "literal_list_input", [&](wp<TSL<TS<i64>, 3>> list) {
        first_sink(first(list));
        second_sink(second(list));
    });

    (void)bg::run_graph(wiring, {{{0, 10}, {1, 15}}, {{1, 20}}});

    CHECK(first_evaluations == 1);
    CHECK(second_evaluations == 2);
}

TEST_CASE("nested time series lists schedule subscribers of changed items",
          "[evaluation][tsl][series:fixed-tsl][scheduling][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    auto first_evaluations = 0;
    auto second_evaluations = 0;

    const auto first_sink = node([&](i64) { ++first_evaluations; });

    const auto second_sink = node([&](i64) { ++second_evaluations; });

    const auto first = node([](In<TSL<TSL<TS<i64>, 2>, 2>> input, Out<TS<i64>> output) {
        if (input.item_updated(0)) {
            output.set(*input[0][0]);
        }
    });

    const auto second = node([](In<TSL<TSL<TS<i64>, 2>, 2>> input, Out<TS<i64>> output) {
        if (input.item_updated(1)) {
            output.set(*input[1][1]);
        }
    });

    const auto wiring = graph("name"_n = "literal_nested_list_input",
                              [&](wp<TSL<TSL<TS<i64>, 2>, 2>> list) {
                                  first_sink(first(list));
                                  second_sink(second(list));
                              });

    (void)bg::run_graph(wiring, {{{0, {{0, 10}}}}, {{1, {{1, 20}}}}});

    CHECK(first_evaluations == 1);
    CHECK(second_evaluations == 1);
}

TEST_CASE("invalidating a list invalidates its items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    auto item_validity = std::vector<bool>{};
    auto required_evaluations = 0;
    auto updated_item_counts = std::vector<std::size_t>{};

    const auto optional_sink = node([&](In<TS<i64>, optional> input) { item_validity.push_back(input.valid()); });

    const auto required_sink = node([&](i64) { ++required_evaluations; });

    const auto list_sink = node([&](In<TSL<TS<i64>, 3>, optional> input) {
        auto count = std::size_t{};
        input.for_each_updated([&](std::size_t) { ++count; });
        updated_item_counts.push_back(count);
    });

    const auto first = node([](In<TSL<TS<i64>, 3>, optional> input, Out<TS<i64>> output) {
        if (input.valid() && input[0].valid()) {
            output.set(*input[0]);
        } else {
            output.invalidate();
        }
    });

    const auto wiring = graph("name"_n = "literal_list_input", [&](wp<TSL<TS<i64>, 3>> list) {
        const auto item = first(list);
        optional_sink(item);
        required_sink(item);
        list_sink(list);
    });

    (void)bg::run_graph(wiring, {{{0, 10}}, literal::INVALID, {{1, 20}}});

    CHECK(item_validity == std::vector{true, false});
    CHECK(required_evaluations == 1);
    CHECK(updated_item_counts == std::vector<std::size_t>{1, 0, 1});
}

TEST_CASE("a list child input reports its invalidation as modified",
          "[evaluation][tsl][series:fixed-tsl][validity][transition:invalidation]"
          "[transition:revalidation][feature:collection-change]") {
    auto states = std::vector<std::array<bool, 4>>{};

    const auto observe = node([&](In<TSL<TS<i64>, 2>> input, In<TS<i64>>, Out<TS<bool>> output) {
        const auto item = input[0];
        states.push_back({item.valid(), item.became_valid(), item.became_invalid(), item.modified()});
        output.set(item.became_invalid() && item.modified());
    });

    const auto graph = [&](wp<TSL<TS<i64>, 2>> input, wp<TS<i64>> trigger) { return observe(input, trigger); };

    const auto actual = bg::run_graph(
        graph, {{{0, 10}, {1, 1}}, {{0, literal::INVALID}}, {{0, 20}}}, {1, 2, 3});

    CHECK(actual.equals({false, true, false}));
    CHECK(states == std::vector<std::array<bool, 4>>{
        {true, true, false, true},
        {false, false, true, true},
        {true, true, false, true},
    });
}

TEST_CASE("a list remains valid while one item remains valid",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    auto item_validity = std::vector<bool>{};
    auto updated_item_counts = std::vector<std::size_t>{};

    const auto item_sink = node([&](In<TS<i64>, optional> input) { item_validity.push_back(input.valid()); });

    const auto list_sink = node([&](In<TSL<TS<i64>, 3>, optional> input) {
        auto count = std::size_t{};
        input.for_each_updated([&](std::size_t) { ++count; });
        updated_item_counts.push_back(count);
    });

    const auto first = node([](In<TSL<TS<i64>, 3>> input, Out<TS<i64>> output) {
        if (input[0].valid()) {
            output.set(*input[0]);
        } else {
            output.invalidate();
        }
    });

    const auto wiring = graph("name"_n = "literal_list_input", [&](wp<TSL<TS<i64>, 3>> list) {
        item_sink(first(list));
        list_sink(list);
    });

    (void)bg::run_graph(wiring, {{{0, 10}, {1, 20}}, {{0, literal::INVALID}}});

    CHECK(item_validity == std::vector{true, false});
    CHECK(updated_item_counts == std::vector<std::size_t>{2, 1});
}

TEST_CASE("a list becomes invalid when its last valid item is invalidated and revalidates on addition",
          "[evaluation][tsl][series:fixed-tsl][feature:validity][validity:optional][feature:collection-change]"
          "[transition:invalidation][transition:revalidation][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto valid = node([](In<TSL<TS<i64>, 3>, optional> input) { return input.valid(); });

    const auto actual = bg::run_graph(valid, {{{0, 10}}, {{0, literal::INVALID}}, {{1, 20}}});

    CHECK(actual.equals({true, false, true}));
}

TEST_CASE("required-input invalidation invalidates list items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][validity:required]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto list = node([](i64 input, Out<TSL<TS<i64>, 3>> output) { output[0].set(input); });

    const auto first = node([](In<TSL<TS<i64>, 3>> input) { return *input[0]; });

    const auto graph = [&](wp<TS<i64>> input) { return first(list(input)); };

    const auto actual = bg::run_graph(graph, {10, literal::INVALID});

    CHECK(actual.equals({10, std::nullopt}));
}

TEST_CASE("nested list invalidation and revalidation propagate through each level",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change]"
          "[transition:invalidation][transition:revalidation][scalar:integer][representation:native]"
          "[declaration:concrete][binding:direct]") {
    struct observation {
        bool outer_valid;
        bool nested_list_valid;
        bool leaf_valid;

        bool operator==(const observation &) const = default;
    };

    const auto inspect = node([](In<TSL<TSL<TS<i64>, 2>, 2>, optional> input) {
        const auto nested_valid = input.valid() && input[0].valid();
        return observation{input.valid(), nested_valid, nested_valid && input[0][0].valid()};
    });

    const auto wiring = graph("name"_n = "literal_nested_list_input",
                              [&](wp<TSL<TSL<TS<i64>, 2>, 2>> input) { return inspect(input); });

    const auto actual = bg::run_graph(wiring, {{{0, {{0, 10}}}}, literal::INVALID, {{0, {{0, 20}}}}});

    CHECK(actual.equals<observation>({observation{true, true, true}, observation{false, false, false},
                                     observation{true, true, true}}));
}

namespace {

struct sequential_item_access_definition {
    Out<TSL<TS<i64>, 2000>> output;

    void start() const {
        for (auto index = std::size_t{}; index < 2000; ++index) {
            [[maybe_unused]] const auto item = output[index];
        }
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("sequential item views reuse evaluation scratch storage",
          "[evaluation][tsl][series:fixed-tsl][storage][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = node<sequential_item_access_definition>();
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        static_cast<void>(source());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto runtime_global_state = GlobalState{allocator{memory}};
    auto runtime = RuntimeGraph{compiled, allocator{memory}, runtime_global_state};
    memory.fail_after(2);

    CHECK_NOTHROW(runtime.start(MIN_DT + EvaluationTime::duration{1}));

    memory.allow_allocations();
    runtime.stop();
}

namespace {

struct canceled_nested_dictionary_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSL<TSD<i64, TS<i64>>, 2>> output;

    void start(EvaluationContext *context) const {
        output[1][0].set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output[0][0].set(10);
        output[0].erase(0);
    }
};

} // namespace

TEST_CASE("inverse nested dictionary mutations publish their containing list",
          "[evaluation][tsl][series:fixed-tsl][tsd][series:tsd][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = node<canceled_nested_dictionary_source_definition>();

    const auto marker = node([](In<TSL<TSD<i64, TS<i64>>, 2>>) { return true; });

    const auto graph = [&] { return marker(source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({true, true}));
}

namespace {

struct retained_nested_item_source_definition {
    Out<TSL<TSL<TS<i64>, 2>, 2>> output;

    void start() const {
        auto item = [&] {
            auto parent = output[0];
            return parent[1];
        }();
        item.set(42);
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("a nested item view retains its containing path",
          "[evaluation][tsl][series:fixed-tsl][storage][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = node<retained_nested_item_source_definition>();

    const auto item = node([](In<TSL<TSL<TS<i64>, 2>, 2>> input) { return *input[0][1]; });

    const auto graph = [&] { return item(source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({42}));
}

TEST_CASE("a switch routes a free fixed list between its items and a constant list",
          "[.][repro][feature:switch][feature:reference][series:fixed-tsl][representation:free]"
          "[shape:nested][binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_free_list",
                                    [](wp<TSL<TS<i64>, 2>> input) { return input; });

    const auto constant = graph("name"_n = "constant_list", [](wp<TSL<TS<i64>, 2>>) {
        return tsl(operators::const_(i64{1}), operators::const_(i64{2}));
    });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto items = tsl(first, second);
        return switch_(key, cases{case_(true, pass_through), case_(false, constant)}, items);
    };

    const auto actual = bg::run_graph(tested,
                                      {false, true, false, true},
                                      {1, literal::SKIP, 3, literal::SKIP},
                                      {-1, literal::SKIP, -3, literal::SKIP});

    auto observed = std::vector<std::string>{};
    for (auto tick_index = std::size_t{}; tick_index < actual.size(); ++tick_index) {
        actual.visit_tick(tick_index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
            observed.push_back(fmt::format("{}", tick(type, valid, reader)));
        });
    }
    CAPTURE(observed);

    CHECK(actual.equals({{{0, 1}, {1, 2}},
                         {{0, 1}, {1, -1}},
                         {{0, 1}, {1, 2}},
                         {{0, 3}, {1, -3}}}));
}

TEST_CASE("a nested graph observes independent updates to a free fixed list",
          "[feature:nested-graph][feature:reference][feature:sampling][series:fixed-tsl]"
          "[representation:free][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto inner = graph("name"_n = "sum_free_list_items", [&](wp<TSL<TS<i64>, 2>> input) {
        return sum(input);
    });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        return nested(inner, run, tsl(first, second));
    };

    const auto actual = bg::run_graph(tested, {1, 2, literal::SKIP}, {10, literal::SKIP, 20}, {true, true, true});

    CHECK(actual.equals({11, 12, 22}));
}

TEST_CASE("a nested graph sees partial validity in a free fixed list",
          "[feature:nested-graph][feature:reference][feature:validity][series:fixed-tsl]"
          "[representation:free][shape:nested][validity:optional][transition:invalidation]"
          "[transition:revalidation]") {
    const auto observe = node([](In<TSL<TS<i64>, 2>, optional> input) {
        return input.valid() && input[0].valid();
    });

    const auto inner = graph("name"_n = "observe_free_list_item", [&](wp<TSL<TS<i64>, 2>> input) {
        return observe(input);
    });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        return nested(inner, run, tsl(first, second));
    };

    const auto actual = bg::run_graph(tested, {1, literal::INVALID, 2}, {10, 11, 12}, {true, true, true});

    CHECK(actual.equals({true, false, true}));
}

TEST_CASE("reselecting a rebuilt free list keeps unchanged item references",
          "[.][repro][feature:switch][feature:reference][feature:sampling][series:fixed-tsl]"
          "[representation:free][shape:nested][binding:reference-to-reference][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_free_list",
                                    [](wp<TSL<TS<i64>, 2>> input) { return input; });

    const auto rebuild = graph("name"_n = "rebuild_free_list", [](wp<TSL<TS<i64>, 2>> input) {
        return tsl(input[0], input[1]);
    });

    const auto first_value = node([](i64 first) { return first; });

    const auto select_first = graph("name"_n = "select_free_list_first", [&](wp<TSL<TS<i64>, 2>> input) {
        return first_value(input[0]);
    });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto items = tsl(first, second);
        return select_first(switch_(key, cases{case_(true, pass_through), case_(false, rebuild)}, items));
    };

    const auto actual = bg::run_graph(tested,
                                      {true, false, true, false},
                                      {10, literal::SKIP, literal::SKIP, 20},
                                      {-1, -2, -3, -4});

    auto observed = std::vector<std::string>{};
    for (auto tick_index = std::size_t{}; tick_index < actual.size(); ++tick_index) {
        actual.visit_tick(tick_index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
            observed.push_back(fmt::format("{}", tick(type, valid, reader)));
        });
    }
    CAPTURE(observed);

    CHECK(actual.equals({10, std::nullopt, std::nullopt, 20}));
}

TEST_CASE("a nested graph passes a free fixed list reference to a value consumer",
          "[.][repro][feature:nested-graph][feature:reference][feature:sampling][series:fixed-tsl]"
          "[representation:free][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto inner = graph("name"_n = "reference_free_list", [](wp<REF<TSL<TS<i64>, 2>>> input) {
        return input;
    });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        return sum(nested(inner, run, tsl(first, second)));
    };

    const auto actual = bg::run_graph(tested, {1, 2, literal::SKIP}, {10, literal::SKIP, 20}, {true, true, true});

    CHECK(actual.equals({11, 12, 22}));
}
