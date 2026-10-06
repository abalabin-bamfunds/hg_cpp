#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

struct set_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSS<std::int64_t>> output;

    void start(bg::EvaluationContext *context) const {
        output.clear();
        schedule_next(context);
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.insert(1);
            output.insert(2);
        } else {
            output.clear();
        }

        if (*step != 2) {
            schedule_next(context);
        }
        ++*step;
    }

  private:
    static void schedule_next(bg::EvaluationContext *context) {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct set_sink_definition {
    bg::In<bg::TSS<std::int64_t>, bg::optional> input;

    void evaluate() const {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_count = std::size_t{};
        input.for_each_added([&](const auto &) { ++added_count; });
        added.push_back(added_count);

        auto removed_count = std::size_t{};
        input.for_each_removed([&](const auto &) { ++removed_count; });
        removed.push_back(removed_count);
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::size_t> added;
    inline static std::vector<std::size_t> removed;
};

} // namespace

TEST_CASE("a time series set becomes valid on its first empty tick and remains valid",
          "[evaluation][tss][series:tss][validity][feature:collection-change][transition:first-tick]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<set_source_definition>();
    const auto sink = bg::node<set_sink_definition>();
    set_sink_definition::validity.clear();
    set_sink_definition::sizes.clear();
    set_sink_definition::added.clear();
    set_sink_definition::removed.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(set_sink_definition::validity == std::vector{true, true, true});
    CHECK(set_sink_definition::sizes == std::vector<std::size_t>{0, 2, 0});
    CHECK(set_sink_definition::added == std::vector<std::size_t>{0, 2, 0});
    CHECK(set_sink_definition::removed == std::vector<std::size_t>{0, 0, 2});
}

namespace {

struct inverse_set_mutation_source_definition {
    bg::Out<bg::TSS<std::int64_t>> output;

    void start() const {
        output.insert(1);
        output.erase(1);
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("adding and removing a set item in one invocation produces a tick",
          "[evaluation][tss][series:tss][feature:collection-change][transition:removal]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct][transition:addition]") {
    const auto source = bg::node<inverse_set_mutation_source_definition>();
    const auto sink = bg::node<set_sink_definition>();
    set_sink_definition::validity.clear();
    set_sink_definition::sizes.clear();
    set_sink_definition::added.clear();
    set_sink_definition::removed.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(set_sink_definition::validity == std::vector{true});
    CHECK(set_sink_definition::sizes == std::vector<std::size_t>{0});
    CHECK(set_sink_definition::added == std::vector<std::size_t>{0});
    CHECK(set_sink_definition::removed == std::vector<std::size_t>{0});
}

namespace {

struct invalidating_value_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct set_from_value_definition {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TSS<std::int64_t>> output;

    void evaluate() const { output.insert(*input); }
};

struct set_size_definition {
    bg::In<bg::TSS<std::int64_t>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(static_cast<int>(input.size())); }
};

} // namespace

TEST_CASE("a time series set preserves its value when a required input becomes invalid",
          "[evaluation][tss][series:tss][validity][feature:collection-change][validity:required]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<invalidating_value_source_definition>();
    const auto set_from_value = bg::node<set_from_value_definition>();
    const auto size = bg::node<set_size_definition>();
    const auto graph = [&] { return size(set_from_value(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({1}));
}

namespace {

struct exact_int_set_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.insert(7);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.erase(7); }
};

struct exact_int_set_contains_definition {
    bg::In<bg::TSS<bg::i64>> input;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const { output.set(input.contains(7)); }
};

} // namespace

TEST_CASE("a time series set accepts its exact non-canonical item type",
          "[evaluation][series:tss][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][transition:addition][transition:removal][binding:direct]") {
    const auto source = bg::node<exact_int_set_source_definition>();
    const auto contains = bg::node<exact_int_set_contains_definition>();
    const auto graph = [&] { return contains(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({true, false}));
}

namespace {

struct referenced_set_source_definition {
    bg::Out<bg::TSS<bg::i64>> output;

    void start() const {
        output.insert(11);
        output.insert(12);
    }

    void evaluate() const {}
};

struct invalid_set_source_definition {
    bg::Out<bg::TSS<bg::i64>> output;

    void evaluate() const {}
};

struct set_reference_condition_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct set_reference_selector_definition {
    bg::In<bg::TS<bg::i64>> condition;
    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive, bg::optional> valid;
    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive, bg::optional> invalid;
    bg::Out<bg::REF<bg::TSS<bg::i64>>> output;

    void evaluate() const { output.copy(*condition == 0 ? valid : invalid); }
};

struct referenced_set_observer_definition {
    bg::In<bg::TSS<bg::i64>, bg::optional> input;

    void evaluate() const {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_items = std::vector<std::int64_t>{};
        input.for_each_added([&](const auto &item) { added_items.push_back(item.value()); });
        added.push_back(std::move(added_items));

        auto removed_items = std::vector<std::int64_t>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::vector<std::int64_t>> added;
    inline static std::vector<std::vector<std::int64_t>> removed;
};

} // namespace

TEST_CASE("a TSS input reports removed items when its reference selects an invalid target",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto valid = bg::node<referenced_set_source_definition>();
    const auto invalid = bg::node<invalid_set_source_definition>();
    const auto condition = bg::node<set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto observe = bg::node<referenced_set_observer_definition>();
    referenced_set_observer_definition::validity.clear();
    referenced_set_observer_definition::sizes.clear();
    referenced_set_observer_definition::added.clear();
    referenced_set_observer_definition::removed.clear();
    const auto graph = [&] { observe(select(condition(), valid(), invalid())); };

    bg::run_graph(graph);

    CHECK(referenced_set_observer_definition::validity == std::vector{true, false});
    CHECK(referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0});
    CHECK(referenced_set_observer_definition::added ==
          std::vector<std::vector<std::int64_t>>{{11, 12}, {}});
    CHECK(referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {11, 12}});
}
