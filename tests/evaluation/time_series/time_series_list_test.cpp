#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
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
    const auto result = testing::test_graph(graph, {1, 2}, {10, 20});

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

    const auto actual = testing::test_graph(wiring, {{{0, 10}, {2, 30}}});

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

    const auto result = testing::test_graph(graph, {1, 2}, {10, literal::SKIP});

    CHECK(result.equals<observation>({observation{11, true, true}, observation{12, true, false}}));
    CHECK(evaluation_count == 2);
}

TEST_CASE("a free fixed time series list follows the receiving item runtime_type",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][reference]"
          "[feature:collection-change][scalar:integer][declaration:concrete][binding:direct]") {
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(tsl(reference(first), second));
    };
    const auto result = testing::test_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({11, 22}));
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

    const auto result = testing::test_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({11, 22}));
}

TEST_CASE("an empty typed TSL publishes valid at graph start",
          "[evaluation][tsl][series:fixed-tsl][feature:wiring-materialisation][representation:free]"
          "[transition:start]") {
    const auto observe = node([](In<TSL<TS<i64>, 0>> input) { return input.valid() && input.size() == 0; });

    const auto graph = [&] { return observe(tsl<TS<i64>>()); };

    CHECK(testing::test_graph(graph).equals({true}));
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

    (void)testing::test_graph(wiring, {{{0, 10}, {1, 15}}, {{1, 20}}});

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

    (void)testing::test_graph(wiring, {{{0, {{0, 10}}}}, {{1, {{1, 20}}}}});

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

    (void)testing::test_graph(wiring, {{{0, 10}}, literal::INVALID, {{1, 20}}});

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

    const auto actual = testing::test_graph(
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

    (void)testing::test_graph(wiring, {{{0, 10}, {1, 20}}, {{0, literal::INVALID}}});

    CHECK(item_validity == std::vector{true, false});
    CHECK(updated_item_counts == std::vector<std::size_t>{2, 1});
}

TEST_CASE("a list becomes invalid when its last valid item is invalidated and revalidates on addition",
          "[evaluation][tsl][series:fixed-tsl][feature:validity][validity:optional][feature:collection-change]"
          "[transition:invalidation][transition:revalidation][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto valid = node([](In<TSL<TS<i64>, 3>, optional> input) { return input.valid(); });

    const auto actual = testing::test_graph(valid, {{{0, 10}}, {{0, literal::INVALID}}, {{1, 20}}});

    CHECK(actual.equals({true, false, true}));
}

TEST_CASE("required-input invalidation invalidates list items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][validity:required]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto list = node([](i64 input, Out<TSL<TS<i64>, 3>> output) { output[0].set(input); });

    const auto first = node([](In<TSL<TS<i64>, 3>> input) { return *input[0]; });

    const auto graph = [&](wp<TS<i64>> input) { return first(list(input)); };

    const auto actual = testing::test_graph(graph, {10, literal::INVALID});

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

    const auto actual = testing::test_graph(wiring, {{{0, {{0, 10}}}}, literal::INVALID, {{0, {{0, 20}}}}});

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

    const auto actual = testing::test_graph(graph);

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

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({42}));
}
