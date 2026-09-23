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

namespace {} // namespace

namespace {

struct list_source_definition {
    bg::Out<bg::TSL<bg::TS<int>, 3>> output;

    void start() const {
        output[0].set(10);
        output[2].set(30);
    }

    void evaluate() const {}
};

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
    bg::In<bg::TSL<bg::TS<int>, 3>> input;

    void evaluate() const {
        values = {*input[0], 0, *input[2]};
        updated = {input.item_updated(0), input.item_updated(1), input.item_updated(2)};
        ++evaluation_count;
    }

    inline static std::array<int, 3> values{};
    inline static std::array<bool, 3> updated{};
    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a fixed time series list publishes changed items",
          "[evaluation][tsl][series:fixed-tsl][feature:collection-change][transition:update]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<list_source_definition>();
    const auto copy = bg::node<generic_list_copy_definition>();
    const auto list_sink = bg::node<list_sink_definition>();
    list_sink_definition::evaluation_count = 0;

    bg::run_graph([&] { list_sink(copy(source())); });

    CHECK(list_sink_definition::evaluation_count == 1);
    CHECK(list_sink_definition::values == std::array{10, 0, 30});
    CHECK(list_sink_definition::updated == std::array{true, false, true});
}

namespace {

struct free_list_sum_definition {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const {
        updates.push_back({input.item_updated(0), input.item_updated(1)});
        output.set(*input[0] + *input[1]);
    }

    inline static std::vector<std::array<bool, 2>> updates;
};

struct free_list_reference_definition {
    bg::In<bg::REF<bg::TS<int>>> input;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void evaluate() const { output.copy(input); }
};

} // namespace

TEST_CASE("a free fixed time series list coalesces source changes",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][feature:collection-change]"
          "[transition:update][scalar:integer][declaration:concrete][binding:direct]") {
    const auto sum = bg::node<free_list_sum_definition>();
    free_list_sum_definition::updates.clear();
    const auto graph = [&](bg::wp<bg::TS<int>> first, bg::wp<bg::TS<int>> second) {
        return sum(bg::tsl(first, second));
    };

    const auto first = std::vector{1, 2};
    const auto second = std::vector<std::optional<int>>{10, std::nullopt};
    const auto result = bg::testing::test_graph(graph, first, second);

    CHECK(result.equals({11, 12}));
    CHECK(free_list_sum_definition::updates == std::vector<std::array<bool, 2>>{{true, true}, {true, false}});
}

TEST_CASE("a free fixed time series list follows the receiving item runtime_type",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][reference]"
          "[feature:collection-change][scalar:integer][declaration:concrete][binding:direct]") {
    const auto reference = bg::node<free_list_reference_definition>();
    const auto sum = bg::node<free_list_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> first, bg::wp<bg::TS<int>> second) {
        return sum(bg::tsl(reference(first), second));
    };
    const auto first = std::vector{1, 2};
    const auto second = std::vector{10, 20};

    const auto result = bg::testing::test_graph(graph, first, second);

    CHECK(result.equals({11, 22}));
}

TEST_CASE("a node can select an item from a free fixed time series list",
          "[evaluation][tsl][feature:getitem][series:fixed-tsl][free][representation:free][scalar:integer]"
          "[declaration:concrete][binding:direct]") {
    const auto second =
        bg::node([](bg::In<bg::TSL<bg::TS<int>, 2>> input, bg::Out<bg::TS<int>> output) { output.set(*input[1]); });
    const auto graph = [&](bg::wp<bg::TS<int>> first, bg::wp<bg::TS<int>> second_value) {
        return second(bg::tsl(first, second_value));
    };
    const auto first = std::vector{1, 2};
    const auto second_values = std::vector{10, 20};

    const auto result = bg::testing::test_graph(graph, first, second_values);

    CHECK(result.equals({10, 20}));
}

namespace {

struct changing_list_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSL<bg::TS<int>, 3>> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        output[1].set(15);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[1].set(20); }
};

struct first_item_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct second_item_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a fixed time series list schedules subscribers of changed items",
          "[evaluation][tsl][series:fixed-tsl][scheduling][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<changing_list_source_definition>();
    const auto first_sink = bg::node<first_item_sink_definition>();
    const auto second_sink = bg::node<second_item_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TS<int>, 3>> input, bg::Out<bg::TS<int>> output) {
        if (input.item_updated(0)) {
            output.set(*input[0]);
        }
    });
    const auto second = bg::node([](bg::In<bg::TSL<bg::TS<int>, 3>> input, bg::Out<bg::TS<int>> output) {
        if (input.item_updated(1)) {
            output.set(*input[1]);
        }
    });
    first_item_sink_definition::evaluation_count = 0;
    second_item_sink_definition::evaluation_count = 0;

    bg::run_graph([&] {
        const auto list = source();
        first_sink(first(list));
        second_sink(second(list));
    });

    CHECK(first_item_sink_definition::evaluation_count == 1);
    CHECK(second_item_sink_definition::evaluation_count == 2);
}

namespace {

struct nested_list_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>> output;

    void start(bg::EvaluationContext *context) const {
        output[0][0].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[1][1].set(20); }
};

struct nested_first_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct nested_second_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("nested time series lists schedule subscribers of changed items",
          "[evaluation][tsl][series:fixed-tsl][scheduling][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<nested_list_source_definition>();
    const auto first_sink = bg::node<nested_first_sink_definition>();
    const auto second_sink = bg::node<nested_second_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>> input, bg::Out<bg::TS<int>> output) {
        if (input.item_updated(0)) {
            output.set(*input[0][0]);
        }
    });
    const auto second = bg::node([](bg::In<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>> input, bg::Out<bg::TS<int>> output) {
        if (input.item_updated(1)) {
            output.set(*input[1][1]);
        }
    });
    nested_first_sink_definition::evaluation_count = 0;
    nested_second_sink_definition::evaluation_count = 0;

    bg::run_graph([&] {
        const auto list = source();
        first_sink(first(list));
        second_sink(second(list));
    });

    CHECK(nested_first_sink_definition::evaluation_count == 1);
    CHECK(nested_second_sink_definition::evaluation_count == 1);
}

namespace {

struct invalidating_list_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSL<bg::TS<int>, 3>> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output[1].set(20);
        }
        ++*step;
    }
};

struct optional_item_sink_definition {
    bg::In<bg::TS<int>, bg::optional> input;

    void evaluate() const { validity.push_back(input.valid()); }

    inline static std::vector<bool> validity;
};

struct required_item_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

struct revalidation_list_sink_definition {
    bg::In<bg::TSL<bg::TS<int>, 3>, bg::optional> input;

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
    const auto source = bg::node<invalidating_list_source_definition>();
    const auto optional_sink = bg::node<optional_item_sink_definition>();
    const auto required_sink = bg::node<required_item_sink_definition>();
    const auto list_sink = bg::node<revalidation_list_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TS<int>, 3>, bg::optional> input, bg::Out<bg::TS<int>> output) {
        if (input.valid() && input[0].valid()) {
            output.set(*input[0]);
        } else {
            output.invalidate();
        }
    });
    optional_item_sink_definition::validity.clear();
    required_item_sink_definition::evaluation_count = 0;
    revalidation_list_sink_definition::updated_item_counts.clear();

    bg::run_graph([&] {
        const auto list = source();
        const auto item = first(list);
        optional_sink(item);
        required_sink(item);
        list_sink(list);
    });

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
    CHECK(required_item_sink_definition::evaluation_count == 1);
    CHECK(revalidation_list_sink_definition::updated_item_counts == std::vector<std::size_t>{1, 0, 1});
}

namespace {

struct partially_invalidating_list_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSL<bg::TS<int>, 3>> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        output[1].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[0].invalidate(); }
};

} // namespace

TEST_CASE("a list remains valid while one item remains valid",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<partially_invalidating_list_source_definition>();
    const auto item_sink = bg::node<optional_item_sink_definition>();
    const auto list_sink = bg::node<revalidation_list_sink_definition>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::TS<int>, 3>> input, bg::Out<bg::TS<int>> output) {
        if (input[0].valid()) {
            output.set(*input[0]);
        } else {
            output.invalidate();
        }
    });
    optional_item_sink_definition::validity.clear();
    revalidation_list_sink_definition::updated_item_counts.clear();

    bg::run_graph([&] {
        const auto list = source();
        item_sink(first(list));
        list_sink(list);
    });

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
    CHECK(revalidation_list_sink_definition::updated_item_counts == std::vector<std::size_t>{2, 1});
}

namespace {

struct invalidating_value_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct list_from_value_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TSL<bg::TS<int>, 3>> output;

    void evaluate() const { output[0].set(*input); }
};

} // namespace

TEST_CASE("required-input invalidation invalidates list items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][validity:required]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<invalidating_value_source_definition>();
    const auto list = bg::node<list_from_value_definition>();
    const auto first =
        bg::node([](bg::In<bg::TSL<bg::TS<int>, 3>> input, bg::Out<bg::TS<int>> output) { output.set(*input[0]); });
    const auto graph = [&] { return first(list(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, std::nullopt}));
}

namespace {

struct invalidating_nested_list_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>> output;

    void start(bg::EvaluationContext *context) const {
        output[0][0].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output[0][0].set(20);
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("nested list invalidation and revalidation propagate through each level",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change]"
          "[transition:invalidation][transition:revalidation][scalar:integer][representation:native]"
          "[declaration:concrete][binding:direct]") {
    const auto source = bg::node<invalidating_nested_list_source_definition>();
    auto validity = std::vector<std::array<bool, 3>>{};
    const auto sink = bg::node([&](bg::In<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>, bg::optional> input) {
        validity.push_back({input.valid(), input.valid() && input[0].valid(),
                            input.valid() && input[0].valid() && input[0][0].valid()});
    });

    bg::run_graph([&] { sink(source()); });

    CHECK(validity == std::vector<std::array<bool, 3>>{{true, true, true}, {false, false, false}, {true, true, true}});
}

namespace {

struct sequential_item_access_definition {
    bg::Out<bg::TSL<bg::TS<int>, 2000>> output;

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

    const auto compiled = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto runtime = bg::RuntimeGraph{compiled, bg::allocator{memory}};
    memory.fail_after(2);

    CHECK_NOTHROW(runtime.start(bg::MIN_DT + bg::EvaluationTime::duration{1}));

    memory.allow_allocations();
    runtime.stop();
}

namespace {

struct canceled_nested_dictionary_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSL<bg::TSD<std::int64_t, bg::TS<int>>, 2>> output;

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
    bg::In<bg::TSL<bg::TSD<std::int64_t, bg::TS<int>>, 2>> input;
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
    bg::Out<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>> output;

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
    const auto item = bg::node([](bg::In<bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>> input, bg::Out<bg::TS<int>> output) {
        output.set(*input[0][1]);
    });
    const auto graph = [&] { return item(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({42}));
}
