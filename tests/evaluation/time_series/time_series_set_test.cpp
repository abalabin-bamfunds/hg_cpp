#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/util/occupancy_index.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace bg::literals;

namespace {

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
    const auto sink = bg::node<set_sink_definition>();
    set_sink_definition::validity.clear();
    set_sink_definition::sizes.clear();
    set_sink_definition::added.clear();
    set_sink_definition::removed.clear();

    const auto graph = bg::graph("name"_n = "literal_set_input",
                                 [&](bg::wp<bg::TSS<std::int64_t>> input) { sink(input); });
    (void)bg::testing::test_graph(graph, {{}, {1, 2}, {literal::REMOVED(1), literal::REMOVED(2)}});

    CHECK(set_sink_definition::validity == std::vector{true, true, true});
    CHECK(set_sink_definition::sizes == std::vector<std::size_t>{0, 2, 0});
    CHECK(set_sink_definition::added == std::vector<std::size_t>{0, 2, 0});
    CHECK(set_sink_definition::removed == std::vector<std::size_t>{0, 0, 2});
}

namespace {

struct composite_literal_set_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSS<bg::List<bg::i64>>> output;

    void start(bg::EvaluationContext *context) const {
        output.apply({{1, 2}});
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.apply({literal::REMOVED({1, 2})}); }
};

struct composite_literal_set_size_definition {
    bg::In<bg::TSS<bg::List<bg::i64>>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(static_cast<bg::i64>(input.size())); }
};

} // namespace

TEST_CASE("a TSS update removes a composite key with an rvalue REMOVED payload",
          "[evaluation][series:tss][feature:collection-change][transition:removal][scalar:list]") {
    const auto source = bg::node<composite_literal_set_source_definition>();
    const auto size = bg::node<composite_literal_set_size_definition>();
    const auto graph = [&] { return size(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<bg::i64>({1, 0}));
}

TEST_CASE("a TSS update applies scalar REMOVED entries in written order",
          "[evaluation][series:tss][feature:collection-change][transition:removal][scalar:integer]") {
    const auto update = bg::node([](bg::Out<bg::TSS<bg::i64>> output) {
        output.apply({1, 2, literal::REMOVED(2), 3});
    });
    const auto project = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        const auto result = (input.contains(1) ? 1 : 0) + (input.contains(2) ? 10 : 0) +
                            (input.contains(3) ? 100 : 0);
        output.set(result);
    });
    const auto graph = [&] { return project(update()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<bg::i64>({101}));
}

TEST_CASE("a TSS update rejects a root INVALID marker",
          "[evaluation][series:tss][feature:collection-change][validity]") {
    const auto source = bg::node([](bg::Out<bg::TSS<bg::i64>> output) { output.apply(literal::INVALID); });

    CHECK_THROWS_AS(bg::run_graph([&] { source(); }), std::invalid_argument);
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

struct clearing_referenced_set_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.insert(11);
        output.insert(12);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.clear(); }
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

struct exact_int_set_contains_definition {
    bg::In<bg::TSS<bg::i64>> input;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const { output.set(input.contains(7)); }
};

} // namespace

TEST_CASE("a time series set accepts its exact non-canonical item type",
          "[evaluation][series:tss][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][transition:addition][transition:removal][binding:direct]") {
    const auto contains = bg::node<exact_int_set_contains_definition>();
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return contains(input); };

    const auto actual = bg::testing::test_graph(graph, {{7}, {literal::REMOVED(7)}});

    CHECK(actual.equals({true, false}));
}

namespace {

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

struct delayed_set_reference_condition_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const { output.set(1); }
};

struct same_cycle_set_reference_condition_definition {
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
    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive, bg::optional> first;
    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive, bg::optional> second;
    bg::Out<bg::REF<bg::TSS<bg::i64>>> output;

    void evaluate() const { output.copy(*condition == 0 ? first : second); }
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
        const auto removed_key = bg::i64{11};
        was_removed.push_back(input.was_removed(bg::Scalar<bg::i64>::from(removed_key)));

        auto cursor_items = std::vector<std::int64_t>{};
        for (auto id = input.next_removed_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            cursor_items.push_back(input.item_at(id).value());
        }
        removed_cursor.push_back(std::move(cursor_items));
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::vector<std::int64_t>> added;
    inline static std::vector<std::vector<std::int64_t>> removed;
    inline static std::vector<bool> was_removed;
    inline static std::vector<std::vector<std::int64_t>> removed_cursor;
};

struct invalidating_set_reference_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive, bg::optional> target;
    bg::Out<bg::REF<bg::TSS<bg::i64>>> output;

    void start(bg::EvaluationContext *context) const {
        output.copy(target);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct set_reference_trigger_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const { output.set(1); }
};

struct passive_referenced_set_observer_definition {
    bg::In<bg::TSS<bg::i64>, bg::passive, bg::optional> input;
    bg::In<bg::TS<bg::i64>> trigger;

    void evaluate() const {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto removed_items = std::vector<std::int64_t>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::vector<std::int64_t>> removed;
};

struct generic_referenced_set_observer_definition {
    bg::In<bg::TS_T, bg::optional> input;

    void evaluate() const {
        input.visit(
            [](const bg::In<bg::TSS<bg::SCALAR>> *set) {
                validity.push_back(set->valid());
                sizes.push_back(set->size());
                auto removed_count = std::size_t{};
                set->for_each_removed([&](const auto &) { ++removed_count; });
                removed.push_back(removed_count);
            },
            ddv::noop);
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::size_t> removed;
};

} // namespace

TEST_CASE("a TSS input reports removed items when its reference selects an invalid target",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto invalid = bg::node<invalid_set_source_definition>();
    const auto condition = bg::node<set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto observe = bg::node<referenced_set_observer_definition>();
    referenced_set_observer_definition::validity.clear();
    referenced_set_observer_definition::sizes.clear();
    referenced_set_observer_definition::added.clear();
    referenced_set_observer_definition::removed.clear();
    referenced_set_observer_definition::was_removed.clear();
    referenced_set_observer_definition::removed_cursor.clear();
    const auto graph = bg::graph("name"_n = "literal_set_input", [&](bg::wp<bg::TSS<bg::i64>> valid) {
        observe(select(condition(), valid, invalid()));
    });

    (void)bg::testing::test_graph(graph, {{11, 12}});

    CHECK(referenced_set_observer_definition::validity == std::vector{true, false});
    CHECK(referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0});
    CHECK(referenced_set_observer_definition::added ==
          std::vector<std::vector<std::int64_t>>{{11, 12}, {}});
    CHECK(referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {11, 12}});
    CHECK(referenced_set_observer_definition::was_removed == std::vector{false, true});
    CHECK(referenced_set_observer_definition::removed_cursor ==
          std::vector<std::vector<std::int64_t>>{{}, {11, 12}});
}

TEST_CASE("a TSS input reports item changes when its reference selects another valid target",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:addition][transition:removal][binding:reference-to-value]"
          "[scalar:integer]") {
    const auto condition = bg::node<set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto observe = bg::node<referenced_set_observer_definition>();
    referenced_set_observer_definition::validity.clear();
    referenced_set_observer_definition::sizes.clear();
    referenced_set_observer_definition::added.clear();
    referenced_set_observer_definition::removed.clear();
    referenced_set_observer_definition::was_removed.clear();
    referenced_set_observer_definition::removed_cursor.clear();
    const auto graph = bg::graph("name"_n = "literal_set_inputs",
                                 [&](bg::wp<bg::TSS<bg::i64>> first, bg::wp<bg::TSS<bg::i64>> second) {
                                     observe(select(condition(), first, second));
                                 });

    (void)bg::testing::test_graph(graph, {{11, 12}}, {{12, 13}});

    CHECK(referenced_set_observer_definition::validity == std::vector{true, true});
    CHECK(referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 2});
    CHECK(referenced_set_observer_definition::added ==
          std::vector<std::vector<std::int64_t>>{{11, 12}, {13}});
    CHECK(referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {11}});
}

TEST_CASE("a sampled TSS reports item ID exchanges across a reference rebind",
          "[evaluation][reference][series:tss][feature:sampling][feature:item-id-remapping]"
          "[binding:reference-to-value][transition:update]") {
    const auto condition = bg::node<set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto inspect = bg::node([](bg::In<bg::TS<bg::i64>> phase, bg::In<bg::TSS<bg::i64>> input,
                                     bg::In<bg::TSS<bg::i64>> first, bg::Out<bg::TS<bg::i64>> output) {
        auto actual = std::vector<std::pair<bg::item_id_t, bg::item_id_t>>{};
        input.for_each_item_id_remapping([&](bg::item_id_t old_id, bg::item_id_t new_id) {
            actual.emplace_back(old_id, new_id);
        });

        auto expected = std::vector<std::pair<bg::item_id_t, bg::item_id_t>>{};
        if (*phase == 1) {
            for (auto new_id = input.next_item_id(0); new_id != bg::util::occupancy_index::npos;
                 new_id = input.next_item_id(new_id + 1)) {
                const auto key = input.item_at(new_id);
                for (auto old_id = first.next_item_id(0); old_id != bg::util::occupancy_index::npos;
                     old_id = first.next_item_id(old_id + 1)) {
                    if (first.item_at(old_id).value() == key.value() && old_id != new_id) {
                        expected.emplace_back(old_id, new_id);
                    }
                }
            }
        }

        std::ranges::sort(actual);
        std::ranges::sort(expected);
        const auto remapping_found = input.has_item_id_remapping();
        const auto valid = actual == expected && remapping_found == !expected.empty() &&
                           (*phase == 0 || expected.size() == 2);
        output.set(valid ? 1 + *phase : 10 + *phase);
    });
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> first, bg::wp<bg::TSS<bg::i64>> second) {
        const auto phase = condition();
        return inspect(phase, select(phase, first, second), first);
    };

    const auto actual = bg::testing::test_graph(graph, {{11, 12}}, {{12, 11}});

    CHECK(actual.equals<std::int64_t>({1, 2}));
}

TEST_CASE("a sampled TSS does not report remapping for additions and removals at unchanged IDs",
          "[evaluation][reference][series:tss][feature:sampling][feature:item-id-remapping]"
          "[binding:reference-to-value][transition:addition][transition:removal]") {
    const auto condition = bg::node<set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto inspect = bg::node([](bg::In<bg::TS<bg::i64>> phase, bg::In<bg::TSS<bg::i64>> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto remapping_count = std::size_t{};
        input.for_each_item_id_remapping([&](bg::item_id_t, bg::item_id_t) { ++remapping_count; });
        const auto remapping_found = input.has_item_id_remapping() || remapping_count != 0;
        output.set(remapping_found ? 10 + *phase : 1 + *phase);
    });
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> first, bg::wp<bg::TSS<bg::i64>> second) {
        const auto phase = condition();
        return inspect(phase, select(phase, first, second));
    };

    const auto actual = bg::testing::test_graph(graph, {{11, 12}}, {{11, 13}});

    CHECK(actual.equals<bg::i64>({1, 2}));
}

TEST_CASE("a TSS reports no item ID remapping for ordinary additions and removals",
          "[evaluation][series:tss][feature:item-id-remapping]"
          "[transition:addition][transition:removal]") {
    const auto inspect = bg::node([](bg::In<bg::TS<bg::i64>> command, bg::In<bg::TSS<bg::i64>> input,
                                     bg::Out<bg::TS<bool>> output) {
        auto remapping_count = std::size_t{};
        input.for_each_item_id_remapping([&](bg::item_id_t, bg::item_id_t) { ++remapping_count; });
        output.set(input.has_item_id_remapping() || remapping_count != 0 || *command == 0);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command, bg::wp<bg::TSS<bg::i64>> input) {
        return inspect(command, input);
    };

    const auto actual = bg::testing::test_graph(graph, {bg::i64{0}, bg::i64{1}},
                                                {{10}, {literal::REMOVED(10), 20}});

    CHECK(actual.equals<bool>({true, false}));
}

TEST_CASE("a TSS input does not report stale removals after an invalid target is rewired",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto valid = bg::node<clearing_referenced_set_source_definition>();
    const auto invalid = bg::node<invalid_set_source_definition>();
    const auto condition = bg::node<delayed_set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto observe = bg::node<referenced_set_observer_definition>();
    referenced_set_observer_definition::validity.clear();
    referenced_set_observer_definition::sizes.clear();
    referenced_set_observer_definition::added.clear();
    referenced_set_observer_definition::removed.clear();
    referenced_set_observer_definition::was_removed.clear();
    referenced_set_observer_definition::removed_cursor.clear();
    const auto graph = [&] { observe(select(condition(), valid(), invalid())); };

    bg::run_graph(graph);

    CHECK(referenced_set_observer_definition::validity == std::vector{true, true, false});
    CHECK(referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0, 0});
    CHECK(referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {11, 12}, {}});
    CHECK(referenced_set_observer_definition::was_removed == std::vector{false, true, false});
}

TEST_CASE("a TSS reference transition uses the old target's current state after a same-cycle clear",
          "[series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto valid = bg::node<clearing_referenced_set_source_definition>();
    const auto invalid = bg::node<invalid_set_source_definition>();
    const auto condition = bg::node<same_cycle_set_reference_condition_definition>();
    const auto select = bg::node<set_reference_selector_definition>();
    const auto observe = bg::node<referenced_set_observer_definition>();
    referenced_set_observer_definition::validity.clear();
    referenced_set_observer_definition::sizes.clear();
    referenced_set_observer_definition::added.clear();
    referenced_set_observer_definition::removed.clear();
    referenced_set_observer_definition::was_removed.clear();
    referenced_set_observer_definition::removed_cursor.clear();
    const auto graph = [&] { observe(select(condition(), valid(), invalid())); };

    bg::run_graph(graph);

    CHECK(referenced_set_observer_definition::validity == std::vector{true, false});
    CHECK(referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0});
    CHECK(referenced_set_observer_definition::added ==
          std::vector<std::vector<std::int64_t>>{{11, 12}, {}});
    CHECK(referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {}});
    CHECK(referenced_set_observer_definition::was_removed == std::vector{false, false});
}

TEST_CASE("a TSS input reports removed items when its reference becomes null",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto reference = bg::node<invalidating_set_reference_definition>();
    const auto observe = bg::node<referenced_set_observer_definition>();
    referenced_set_observer_definition::validity.clear();
    referenced_set_observer_definition::sizes.clear();
    referenced_set_observer_definition::added.clear();
    referenced_set_observer_definition::removed.clear();
    referenced_set_observer_definition::was_removed.clear();
    referenced_set_observer_definition::removed_cursor.clear();
    const auto graph = bg::graph("name"_n = "literal_set_input",
                                 [&](bg::wp<bg::TSS<bg::i64>> source) { observe(reference(source)); });

    (void)bg::testing::test_graph(graph, {{11, 12}});

    CHECK(referenced_set_observer_definition::validity == std::vector{true, false});
    CHECK(referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0});
    CHECK(referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {11, 12}});
    CHECK(referenced_set_observer_definition::was_removed == std::vector{false, true});
    CHECK(referenced_set_observer_definition::removed_cursor ==
          std::vector<std::vector<std::int64_t>>{{}, {11, 12}});
}

TEST_CASE("a passive TSS input does not retain a stale removal sample",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][activity:passive][transition:removal][binding:reference-to-value]"
          "[scalar:integer]") {
    const auto reference = bg::node<invalidating_set_reference_definition>();
    const auto trigger = bg::node<set_reference_trigger_definition>();
    const auto observe = bg::node<passive_referenced_set_observer_definition>();
    passive_referenced_set_observer_definition::validity.clear();
    passive_referenced_set_observer_definition::sizes.clear();
    passive_referenced_set_observer_definition::removed.clear();
    const auto graph = bg::graph("name"_n = "literal_set_input", [&](bg::wp<bg::TSS<bg::i64>> source) {
        observe(reference(source), trigger());
    });

    (void)bg::testing::test_graph(graph, {{11, 12}});

    CHECK(passive_referenced_set_observer_definition::validity == std::vector{true, false});
    CHECK(passive_referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0});
    CHECK(passive_referenced_set_observer_definition::removed ==
          std::vector<std::vector<std::int64_t>>{{}, {}});
}

TEST_CASE("a generic input visitor sees TSS removals on reference invalidation",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]"
          "[declaration:generic]") {
    const auto reference = bg::node<invalidating_set_reference_definition>();
    const auto observe = bg::node<generic_referenced_set_observer_definition>();
    generic_referenced_set_observer_definition::validity.clear();
    generic_referenced_set_observer_definition::sizes.clear();
    generic_referenced_set_observer_definition::removed.clear();
    const auto graph = bg::graph("name"_n = "literal_set_input",
                                 [&](bg::wp<bg::TSS<bg::i64>> source) { observe(reference(source)); });

    (void)bg::testing::test_graph(graph, {{11, 12}});

    CHECK(generic_referenced_set_observer_definition::validity == std::vector{true, false});
    CHECK(generic_referenced_set_observer_definition::sizes == std::vector<std::size_t>{2, 0});
    CHECK(generic_referenced_set_observer_definition::removed == std::vector<std::size_t>{0, 2});
}
