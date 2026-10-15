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

using namespace bg::literals;

namespace {

struct generic_list_copy_definition {
    bg::In<bg::TSL<bg::TS<bg::SCALAR>, 3>> input;
    bg::Out<bg::TSL<bg::TS<bg::SCALAR>, 3>> output;

    void evaluate() const {
        for (auto item = std::size_t{}; item < 3; ++item) {
            if (input[item].valid()) {
                output[item].set_value(input[item].value_view());
            }
        }
    }
};

struct list_sink_definition {
    bg::In<bg::TSL<bg::TS<bg::i64>, 3>> input;

    void evaluate() const {
        values = {*input[0], 0, *input[2]};
        updated = {input.item_updated(0), input.item_updated(1), input.item_updated(2)};
        ++evaluation_count;
    }

    inline static std::array<bg::i64, 3> values{};
    inline static std::array<bool, 3> updated{};
    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a fixed time series list publishes changed items",
          "[evaluation][tsl][series:fixed-tsl][feature:collection-change][transition:update]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto copy = bg::node<generic_list_copy_definition>();
    const auto list_sink = bg::node<list_sink_definition>();
    list_sink_definition::evaluation_count = 0;
    const auto graph = bg::graph("name"_n = "literal_list_input",
                                 [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 3>> input) { list_sink(copy(input)); });

    (void)bg::testing::test_graph(graph, {{{0, 10}, {2, 30}}});

    CHECK(list_sink_definition::evaluation_count == 1);
    CHECK(list_sink_definition::values == std::array<bg::i64, 3>{10, 0, 30});
    CHECK(list_sink_definition::updated == std::array{true, false, true});
}

namespace {

struct free_list_sum_definition {
    bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        updates.push_back({input.item_updated(0), input.item_updated(1)});
        output.set(*input[0] + *input[1]);
    }

    inline static std::vector<std::array<bool, 2>> updates;
};

struct free_list_reference_definition {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;

    void evaluate() const { output.copy(input); }
};

} // namespace

TEST_CASE("a free fixed time series list coalesces source changes",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][feature:collection-change]"
          "[transition:update][scalar:integer][declaration:concrete][binding:direct]") {
    const auto sum = bg::node<free_list_sum_definition>();
    free_list_sum_definition::updates.clear();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second) {
        return sum(bg::tsl(first, second));
    };

    const auto result = bg::testing::test_graph(graph, {1, 2}, {10, literal::SKIP});

    CHECK(result.equals({11, 12}));
    CHECK(free_list_sum_definition::updates == std::vector<std::array<bool, 2>>{{true, true}, {true, false}});
}

TEST_CASE("a free fixed time series list follows the receiving item runtime_type",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][reference]"
          "[feature:collection-change][scalar:integer][declaration:concrete][binding:direct]") {
    const auto reference = bg::node<free_list_reference_definition>();
    const auto sum = bg::node<free_list_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second) {
        return sum(bg::tsl(reference(first), second));
    };
    const auto result = bg::testing::test_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({11, 22}));
}

TEST_CASE("a node can select an item from a free fixed time series list",
          "[evaluation][tsl][feature:getitem][series:fixed-tsl][free][representation:free][scalar:integer]"
          "[declaration:concrete][binding:direct]") {
    const auto second =
        bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input, bg::Out<bg::TS<bg::i64>> output) { output.set(*input[1]); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second_value) {
        return second(bg::tsl(first, second_value));
    };
    const auto result = bg::testing::test_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({10, 20}));
}

TEST_CASE("a span-backed free TSL retains its items through materialisation and evaluation",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][feature:wiring-materialisation]"
          "[scalar:integer]") {
    const auto sum = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input,
                                 bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second) {
        const auto collection = [&] {
            const auto items = std::vector<bg::WiringPoint>{first, second};
            return bg::tsl(std::span<const bg::WiringPoint>{items});
        }();
        return sum(collection);
    };

    const auto result = bg::testing::test_graph(graph, {1, 2}, {10, 20});

    CHECK(result.equals({11, 22}));
}

TEST_CASE("an empty typed TSL publishes valid at graph start",
          "[evaluation][tsl][series:fixed-tsl][feature:wiring-materialisation][representation:free]"
          "[transition:start]") {
    const auto observe = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 0>> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.valid() && input.size() == 0 ? 1 : 0);
    });
    const auto graph = [&] { return observe(bg::tsl<bg::TS<bg::i64>>()); };

    CHECK(bg::testing::test_graph(graph).equals({1}));
}

namespace {

struct first_item_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct second_item_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a fixed time series list schedules subscribers of changed items",
          "[evaluation][tsl][series:fixed-tsl][scheduling][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto first_sink = bg::node<first_item_sink_definition>();
    const auto second_sink = bg::node<second_item_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 3>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (input.item_updated(0)) {
            output.set(*input[0]);
        }
    });
    const auto second = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 3>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (input.item_updated(1)) {
            output.set(*input[1]);
        }
    });
    first_item_sink_definition::evaluation_count = 0;
    second_item_sink_definition::evaluation_count = 0;

    const auto graph = bg::graph("name"_n = "literal_list_input", [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 3>> list) {
        first_sink(first(list));
        second_sink(second(list));
    });

    (void)bg::testing::test_graph(graph, {{{0, 10}, {1, 15}}, {{1, 20}}});

    CHECK(first_item_sink_definition::evaluation_count == 1);
    CHECK(second_item_sink_definition::evaluation_count == 2);
}

namespace {

struct nested_first_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct nested_second_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("nested time series lists schedule subscribers of changed items",
          "[evaluation][tsl][series:fixed-tsl][scheduling][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto first_sink = bg::node<nested_first_sink_definition>();
    const auto second_sink = bg::node<nested_second_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (input.item_updated(0)) {
            output.set(*input[0][0]);
        }
    });
    const auto second = bg::node([](bg::In<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (input.item_updated(1)) {
            output.set(*input[1][1]);
        }
    });
    nested_first_sink_definition::evaluation_count = 0;
    nested_second_sink_definition::evaluation_count = 0;

    const auto graph = bg::graph("name"_n = "literal_nested_list_input",
                                 [&](bg::wp<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>> list) {
                                     first_sink(first(list));
                                     second_sink(second(list));
                                 });

    (void)bg::testing::test_graph(graph, {{{0, {{0, 10}}}}, {{1, {{1, 20}}}}});

    CHECK(nested_first_sink_definition::evaluation_count == 1);
    CHECK(nested_second_sink_definition::evaluation_count == 1);
}

namespace {

struct optional_item_sink_definition {
    bg::In<bg::TS<bg::i64>, bg::optional> input;

    void evaluate() const { validity.push_back(input.valid()); }

    inline static std::vector<bool> validity;
};

struct required_item_sink_definition {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct revalidation_list_sink_definition {
    bg::In<bg::TSL<bg::TS<bg::i64>, 3>, bg::optional> input;

    void evaluate() const {
        auto count = std::size_t{};
        input.for_each_updated([&](std::size_t) { ++count; });
        updated_item_counts.push_back(count);
    }

    inline static std::vector<std::size_t> updated_item_counts;
};

} // namespace

TEST_CASE("invalidating a list invalidates its items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto optional_sink = bg::node<optional_item_sink_definition>();
    const auto required_sink = bg::node<required_item_sink_definition>();
    const auto list_sink = bg::node<revalidation_list_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 3>, bg::optional> input, bg::Out<bg::TS<bg::i64>> output) {
        if (input.valid() && input[0].valid()) {
            output.set(*input[0]);
        } else {
            output.invalidate();
        }
    });
    optional_item_sink_definition::validity.clear();
    required_item_sink_definition::evaluation_count = 0;
    revalidation_list_sink_definition::updated_item_counts.clear();

    const auto graph = bg::graph("name"_n = "literal_list_input", [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 3>> list) {
        const auto item = first(list);
        optional_sink(item);
        required_sink(item);
        list_sink(list);
    });

    (void)bg::testing::test_graph(graph, {{{0, 10}}, literal::INVALID, {{1, 20}}});

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
    CHECK(required_item_sink_definition::evaluation_count == 1);
    CHECK(revalidation_list_sink_definition::updated_item_counts == std::vector<std::size_t>{1, 0, 1});
}

TEST_CASE("a list remains valid while one item remains valid",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto item_sink = bg::node<optional_item_sink_definition>();
    const auto list_sink = bg::node<revalidation_list_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 3>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (input[0].valid()) {
            output.set(*input[0]);
        } else {
            output.invalidate();
        }
    });
    optional_item_sink_definition::validity.clear();
    revalidation_list_sink_definition::updated_item_counts.clear();

    const auto graph = bg::graph("name"_n = "literal_list_input", [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 3>> list) {
        item_sink(first(list));
        list_sink(list);
    });

    (void)bg::testing::test_graph(graph, {{{0, 10}, {1, 20}}, {{0, literal::INVALID}}});

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
    CHECK(revalidation_list_sink_definition::updated_item_counts == std::vector<std::size_t>{2, 1});
}

namespace {

struct list_from_value_definition {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TSL<bg::TS<bg::i64>, 3>> output;

    void evaluate() const { output[0].set(*input); }
};

} // namespace

TEST_CASE("required-input invalidation invalidates list items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][validity:required]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto list = bg::node<list_from_value_definition>();
    const auto first =
        bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 3>> input, bg::Out<bg::TS<bg::i64>> output) { output.set(*input[0]); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return first(list(input)); };

    const auto actual = bg::testing::test_graph(graph, {10, literal::INVALID});

    CHECK(actual.equals({10, std::nullopt}));
}

TEST_CASE("nested list invalidation and revalidation propagate through each level",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change]"
          "[transition:invalidation][transition:revalidation][scalar:integer][representation:native]"
          "[declaration:concrete][binding:direct]") {
    auto validity = std::vector<std::array<bool, 3>>{};
    const auto sink = bg::node([&](bg::In<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>, bg::optional> input) {
        validity.push_back({input.valid(), input.valid() && input[0].valid(),
                            input.valid() && input[0].valid() && input[0][0].valid()});
    });

    const auto graph = bg::graph("name"_n = "literal_nested_list_input",
                                 [&](bg::wp<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>> input) { sink(input); });

    (void)bg::testing::test_graph(graph, {{{0, {{0, 10}}}}, literal::INVALID, {{0, {{0, 20}}}}});

    CHECK(validity == std::vector<std::array<bool, 3>>{{true, true, true}, {false, false, false}, {true, true, true}});
}

namespace {

struct sequential_item_access_definition {
    bg::Out<bg::TSL<bg::TS<bg::i64>, 2000>> output;

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
    const auto source = bg::node<sequential_item_access_definition>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        static_cast<void>(source());
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto runtime_global_state = bg::GlobalState{bg::allocator{memory}};
    auto runtime = bg::RuntimeGraph{compiled, bg::allocator{memory}, runtime_global_state};
    memory.fail_after(2);

    CHECK_NOTHROW(runtime.start(bg::MIN_DT + bg::EvaluationTime::duration{1}));

    memory.allow_allocations();
    runtime.stop();
}

namespace {

struct canceled_nested_dictionary_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSL<bg::TSD<std::int64_t, bg::TS<bg::i64>>, 2>> output;

    void start(bg::EvaluationContext *context) const {
        output[1][0].set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output[0][0].set(10);
        output[0].erase(0);
    }
};

struct list_evaluation_marker_definition {
    bg::In<bg::TSL<bg::TSD<std::int64_t, bg::TS<bg::i64>>, 2>> input;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const { output.set(true); }
};

} // namespace

TEST_CASE("inverse nested dictionary mutations publish their containing list",
          "[evaluation][tsl][series:fixed-tsl][tsd][series:tsd][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<canceled_nested_dictionary_source_definition>();
    const auto marker = bg::node<list_evaluation_marker_definition>();
    const auto graph = [&] { return marker(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({true, true}));
}

namespace {

struct retained_nested_item_source_definition {
    bg::Out<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>> output;

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
    const auto source = bg::node<retained_nested_item_source_definition>();
    const auto item = bg::node([](bg::In<bg::TSL<bg::TSL<bg::TS<bg::i64>, 2>, 2>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input[0][1]);
    });
    const auto graph = [&] { return item(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({42}));
}
