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
#include <vector>

namespace {

using IntTimeSeriesList = bg::TSL<bg::TS<int>, 3>;

} // namespace

namespace {

struct list_source_definition {
    bg::Out<IntTimeSeriesList> output;

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
    bg::In<IntTimeSeriesList> input;

    void evaluate() const {
        values = {*input[0], 0, *input[2]};
        updated = {input.item_updated(0), input.item_updated(1), input.item_updated(2)};
        ++evaluation_count;
    }

    inline static std::array<int, 3> values{};
    inline static std::array<bool, 3> updated{};
    inline static int evaluation_count{};
};

struct item_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const {
        value = *input;
        ++evaluation_count;
    }

    inline static int value{};
    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a fixed time series list publishes changed items",
          "[evaluation][tsl][series:fixed-tsl][feature:collection-change][transition:update]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<list_source_definition>();
    const auto copy = bg::node<generic_list_copy_definition>();
    const auto list_sink = bg::node<list_sink_definition>();
    const auto item_sink = bg::node<item_sink_definition>();
    list_sink_definition::evaluation_count = 0;
    item_sink_definition::evaluation_count = 0;

    bg::run_graph([&] {
        const auto list = copy(source());
        list_sink(list);
        item_sink(list[2]);
    });

    CHECK(list_sink_definition::evaluation_count == 1);
    CHECK(list_sink_definition::values == std::array{10, 0, 30});
    CHECK(list_sink_definition::updated == std::array{true, false, true});
    CHECK(item_sink_definition::evaluation_count == 1);
    CHECK(item_sink_definition::value == 30);
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

    void evaluate() const { output.set(input); }
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

TEST_CASE("a graph can select the original item from a free fixed time series list",
          "[evaluation][tsl][series:fixed-tsl][free][representation:free][wiring][scalar:integer]"
          "[declaration:concrete][binding:direct][feature:wiring]") {
    using namespace bg::literals;
    const auto second =
        bg::graph("name"_m = "second_item",
                  [](bg::wp<bg::TSL<bg::TS<int>, 2>> input) -> bg::wp<bg::TS<int>> { return input[1]; });
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
    static constexpr bool uses_future_scheduling = true;

    bg::Out<IntTimeSeriesList> output;

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
    first_item_sink_definition::evaluation_count = 0;
    second_item_sink_definition::evaluation_count = 0;

    bg::run_graph([&] {
        const auto list = source();
        first_sink(list[0]);
        second_sink(list[1]);
    });

    CHECK(first_item_sink_definition::evaluation_count == 1);
    CHECK(second_item_sink_definition::evaluation_count == 2);
}

namespace {

using NestedIntTimeSeriesList = bg::TSL<bg::TSL<bg::TS<int>, 2>, 2>;

struct nested_list_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<NestedIntTimeSeriesList> output;

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
    nested_first_sink_definition::evaluation_count = 0;
    nested_second_sink_definition::evaluation_count = 0;

    bg::run_graph([&] {
        const auto list = source();
        first_sink(list[0][0]);
        second_sink(list[1][1]);
    });

    CHECK(nested_first_sink_definition::evaluation_count == 1);
    CHECK(nested_second_sink_definition::evaluation_count == 1);
}

namespace {

struct invalidating_list_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<IntTimeSeriesList> output;

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
    bg::In<IntTimeSeriesList, bg::optional> input;

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
    optional_item_sink_definition::validity.clear();
    required_item_sink_definition::evaluation_count = 0;
    revalidation_list_sink_definition::updated_item_counts.clear();

    bg::run_graph([&] {
        const auto list = source();
        optional_sink(list[0]);
        required_sink(list[0]);
        list_sink(list);
    });

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
    CHECK(required_item_sink_definition::evaluation_count == 1);
    CHECK(revalidation_list_sink_definition::updated_item_counts == std::vector<std::size_t>{1, 0, 1});
}

namespace {

struct partially_invalidating_list_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<IntTimeSeriesList> output;

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
    optional_item_sink_definition::validity.clear();
    revalidation_list_sink_definition::updated_item_counts.clear();

    bg::run_graph([&] {
        const auto list = source();
        item_sink(list[0]);
        list_sink(list);
    });

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
    CHECK(revalidation_list_sink_definition::updated_item_counts == std::vector<std::size_t>{2, 1});
}

namespace {

struct invalidating_value_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct list_from_value_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<IntTimeSeriesList> output;

    void evaluate() const { output[0].set(*input); }
};

} // namespace

TEST_CASE("required-input invalidation invalidates list items",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change][validity:required]"
          "[transition:invalidation][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<invalidating_value_source_definition>();
    const auto list = bg::node<list_from_value_definition>();
    const auto sink = bg::node<optional_item_sink_definition>();
    optional_item_sink_definition::validity.clear();

    bg::run_graph([&] { sink(list(source())[0]); });

    CHECK(optional_item_sink_definition::validity == std::vector{true, false});
}

namespace {

struct invalidating_nested_list_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<NestedIntTimeSeriesList> output;

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

template <typename TimeSeries> struct validity_sink_definition {
    bg::In<TimeSeries, bg::optional> input;

    void evaluate() const { validity.push_back(input.valid()); }

    inline static std::vector<bool> validity;
};

} // namespace

TEST_CASE("nested list invalidation and revalidation propagate through each level",
          "[evaluation][tsl][series:fixed-tsl][validity][feature:collection-change]"
          "[transition:invalidation][transition:revalidation][scalar:integer][representation:native]"
          "[declaration:concrete][binding:direct]") {
    using InnerIntTimeSeriesList = bg::TSL<bg::TS<int>, 2>;
    using InnerValiditySink = validity_sink_definition<InnerIntTimeSeriesList>;
    using OuterValiditySink = validity_sink_definition<NestedIntTimeSeriesList>;
    using ItemValiditySink = validity_sink_definition<bg::TS<int>>;

    const auto source = bg::node<invalidating_nested_list_source_definition>();
    const auto inner_sink = bg::node<InnerValiditySink>();
    const auto outer_sink = bg::node<OuterValiditySink>();
    const auto item_sink = bg::node<ItemValiditySink>();
    InnerValiditySink::validity.clear();
    OuterValiditySink::validity.clear();
    ItemValiditySink::validity.clear();

    bg::run_graph([&] {
        const auto list = source();
        inner_sink(list[0]);
        outer_sink(list);
        item_sink(list[0][0]);
    });

    CHECK(InnerValiditySink::validity == std::vector{true, false, true});
    CHECK(OuterValiditySink::validity == std::vector{true, false, true});
    CHECK(ItemValiditySink::validity == std::vector{true, false, true});
}

namespace {

struct passive_item_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<IntTimeSeriesList> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        output[1].set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output[0].invalidate();
            output[1].set(1);
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output[0].set(20);
            output[1].set(2);
        }
        ++*step;
    }
};

struct passive_item_sink_definition {
    bg::In<bg::TS<int>, bg::passive, bg::optional> item;
    bg::In<bg::TS<int>> trigger;

    void evaluate() const {
        validity.push_back(item.valid());
        if (item.valid()) {
            values.push_back(*item);
        }
    }

    inline static std::vector<bool> validity;
    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("a passive optional list item observes validity when another item schedules its node",
          "[evaluation][tsl][series:fixed-tsl][scheduling][validity][feature:collection-change]"
          "[activity:passive][validity:optional][scalar:integer][representation:native]"
          "[declaration:concrete][binding:direct]") {
    const auto source = bg::node<passive_item_source_definition>();
    const auto sink = bg::node<passive_item_sink_definition>();
    passive_item_sink_definition::validity.clear();
    passive_item_sink_definition::values.clear();

    bg::run_graph([&] {
        const auto list = source();
        sink(list[0], list[1]);
    });

    CHECK(passive_item_sink_definition::validity == std::vector{true, false, true});
    CHECK(passive_item_sink_definition::values == std::vector{10, 20});
}

namespace {

using LargeIntTimeSeriesList = bg::TSL<bg::TS<int>, 2000>;

struct sequential_item_access_definition {
    bg::Out<LargeIntTimeSeriesList> output;

    void start() const {
        for (auto index = std::size_t{}; index < 2000; ++index) {
            const auto item = output[index];
            static_cast<void>(item.valid());
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
    auto runtime = bg::RuntimeGraph{compiled, memory};
    memory.fail_after(2);

    CHECK_NOTHROW(runtime.start(bg::MIN_DT + bg::EvaluationTime::duration{1}));

    memory.allow_allocations();
    runtime.stop();
}

namespace {

using IntTimeSeriesDictionary = bg::TSD<std::int64_t, bg::TS<int>>;
using DictionaryTimeSeriesList = bg::TSL<IntTimeSeriesDictionary, 2>;

struct canceled_nested_dictionary_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<DictionaryTimeSeriesList> output;

    void start(bg::EvaluationContext *context) const {
        output[1][0].set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output[0][0].set(10);
        output[0].erase(0);
    }
};

struct list_evaluation_count_sink_definition {
    bg::In<DictionaryTimeSeriesList> input;

    void evaluate() const { ++count; }

    inline static std::size_t count{};
};

} // namespace

TEST_CASE("inverse nested dictionary mutations publish their containing list",
          "[evaluation][tsl][series:fixed-tsl][tsd][series:tsd][feature:collection-change]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<canceled_nested_dictionary_source_definition>();
    const auto sink = bg::node<list_evaluation_count_sink_definition>();
    list_evaluation_count_sink_definition::count = 0;

    bg::run_graph([&] { sink(source()); });

    CHECK(list_evaluation_count_sink_definition::count == 2);
}

namespace {

struct retained_nested_item_source_definition {
    bg::Out<NestedIntTimeSeriesList> output;

    void start() const {
        auto item = [&] {
            auto parent = output[0];
            return parent[1];
        }();
        item.set(42);
    }

    void evaluate() const {}
};

struct retained_nested_item_sink_definition {
    bg::In<bg::TS<int>> input;

    void evaluate() const { value = *input; }

    inline static int value{};
};

} // namespace

TEST_CASE("a nested item view retains its containing path",
          "[evaluation][tsl][series:fixed-tsl][storage][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<retained_nested_item_source_definition>();
    const auto sink = bg::node<retained_nested_item_sink_definition>();
    retained_nested_item_sink_definition::value = 0;

    bg::run_graph([&] { sink(source()[0][1]); });

    CHECK(retained_nested_item_sink_definition::value == 42);
}
