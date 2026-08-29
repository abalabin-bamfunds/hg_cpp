#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/list/type.hpp>

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
        values = {input[0].value(), 0, input[2].value()};
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
        value = input.value();
        ++evaluation_count;
    }

    inline static int value{};
    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a fixed time series list publishes changed items", "[evaluation][tsl]") {
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

struct changing_list_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<IntTimeSeriesList> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        output[1].set(15);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
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

TEST_CASE("a fixed time series list schedules subscribers of changed items", "[evaluation][tsl][scheduling]") {
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
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

TEST_CASE("nested time series lists schedule subscribers of changed items", "[evaluation][tsl][scheduling]") {
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
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

TEST_CASE("invalidating a list invalidates its items", "[evaluation][tsl][validity]") {
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[0].invalidate(); }
};

} // namespace

TEST_CASE("a list remains valid while one item remains valid", "[evaluation][tsl][validity]") {
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct list_from_value_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<IntTimeSeriesList> output;

    void evaluate() const { output[0].set(input.value()); }
};

} // namespace

TEST_CASE("required-input invalidation invalidates list items", "[evaluation][tsl][validity]") {
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
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

TEST_CASE("nested list invalidation and revalidation propagate through each level", "[evaluation][tsl][validity]") {
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output[0].invalidate();
            output[1].set(1);
            (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
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
            values.push_back(item.value());
        }
    }

    inline static std::vector<bool> validity;
    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("a passive optional list item observes validity when another item schedules its node",
          "[evaluation][tsl][scheduling][validity]") {
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
