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

using namespace bg;
using namespace literals;

TEST_CASE("a time series set publishes only new items",
          "[feature:collection-change][series:tss][scalar:integer][transition:addition][shape:single-node]") {
    const auto collect = node([](i64 value, Out<TSS<i64>> output) { output.insert(value); });
    const auto actual = testing::test_graph(collect, {1, 2, 1});

    CHECK(actual.equals({{1}, {2}, literal::SKIP}));
}

TEST_CASE("a time series set becomes valid on its first empty tick and remains valid",
          "[evaluation][tss][series:tss][validity][feature:collection-change][transition:first-tick]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    struct observation {
        bool valid;
        i64 size;
        i64 added;
        i64 removed;

        bool operator==(const observation &) const = default;
    };

    const auto observe = node([](In<TSS<i64>, optional> input) {
        auto added = i64{};
        input.for_each_added([&](const auto &) { ++added; });

        auto removed = i64{};
        input.for_each_removed([&](const auto &) { ++removed; });

        return observation{input.valid(), static_cast<i64>(input.size()), added, removed};
    });

    const auto wiring = graph("name"_n = "literal_set_input", [&](wp<TSS<i64>> input) { return observe(input); });
    const auto actual = testing::test_graph(wiring, {{}, {1, 2}, {literal::REMOVED(1), literal::REMOVED(2)}});

    CHECK(actual.equals<observation>({observation{true, 0, 0, 0}, observation{true, 2, 2, 0},
                                     observation{true, 0, 0, 2}}));
}

namespace {

struct tss_membership_evaluation_count_definition {
    static constexpr bool uses_cycle_callbacks = true;

    bg::In<bg::REF<bg::TSS<bg::i64>>> collection;
    bg::In<bg::TS<bool>, bg::optional, bg::dynamic_binding> membership;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const {
        ++evaluation_count;
        const auto key = bg::i64{7};
        membership.bind(collection.membership(bg::Scalar<bg::i64>::from(key)));
        output.copy(membership);
    }

    void on_invalidation() const {
        membership.unbind();
        output.invalidate();
    }

    inline static std::size_t evaluation_count{};
};

} // namespace

TEST_CASE("a TSS membership binding ignores changes to other keys",
          "[evaluation][tss][series:tss][series:reference][feature:collection-change]"
          "[scalar:integer][transition:addition][transition:removal][definition:aggregate]") {
    const auto observe = bg::node<tss_membership_evaluation_count_definition>();
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return observe(input); };
    tss_membership_evaluation_count_definition::evaluation_count = 0;

    const auto actual = bg::testing::test_graph(graph, {{7}, {8}, {literal::REMOVED(7)}, {7}});

    CHECK(actual.equals<bool>({true, std::nullopt, false, true}));
    CHECK(tss_membership_evaluation_count_definition::evaluation_count == 3);
}

namespace {

struct composite_literal_set_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSS<List<i64>>> output;

    void start(EvaluationContext *context) const {
        output.apply({{1, 2}});
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.apply({literal::REMOVED({1, 2})}); }
};

} // namespace

TEST_CASE("a TSS update removes a composite key with an rvalue REMOVED payload",
          "[evaluation][series:tss][feature:collection-change][transition:removal][scalar:list]") {
    const auto source = node<composite_literal_set_source_definition>();

    const auto size = node([](In<TSS<List<i64>>> input) { return static_cast<i64>(input.size()); });

    const auto graph = [&] { return size(source()); };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals<i64>({1, 0}));
}

TEST_CASE("a TSS update applies scalar REMOVED entries in written order",
          "[evaluation][series:tss][feature:collection-change][transition:removal][scalar:integer]") {
    const auto update = node([](Out<TSS<i64>> output) {
        output.apply({1, 2, literal::REMOVED(2), 3});
    });

    const auto actual = testing::test_graph(update);

    CHECK(actual.equals({{1, 3}}));
}

TEST_CASE("a TSS update can reinsert an item after removing it in the same tick",
          "[evaluation][series:tss][feature:collection-change][transition:addition][transition:removal]"
          "[scalar:integer]") {
    const auto update = node([](Out<TSS<i64>> output) {
        output.apply({1, 2, literal::REMOVED(2), 2, 3});
    });

    const auto actual = testing::test_graph(update);

    CHECK(actual.equals({{1, 2, 3}}));
}

namespace {

struct inverse_set_mutation_source_definition {
    Out<TSS<i64>> output;

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
    struct observation {
        bool valid;
        i64 size;
        i64 added;
        i64 removed;

        bool operator==(const observation &) const = default;
    };

    const auto sink = node([](In<TSS<i64>, optional> input) {
        auto added = i64{};
        input.for_each_added([&](const auto &) { ++added; });

        auto removed = i64{};
        input.for_each_removed([&](const auto &) { ++removed; });

        return observation{input.valid(), static_cast<i64>(input.size()), added, removed};
    });

    const auto source = node<inverse_set_mutation_source_definition>();

    const auto actual = testing::test_graph([&] { return sink(source()); });

    CHECK(actual.equals<observation>({observation{true, 0, 0, 0}}));
}

namespace {

struct invalidating_value_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct clearing_referenced_set_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSS<i64>> output;

    void start(EvaluationContext *context) const {
        output.insert(11);
        output.insert(12);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.clear(); }
};

} // namespace

TEST_CASE("a time series set preserves its value when a required input becomes invalid",
          "[evaluation][tss][series:tss][validity][feature:collection-change][validity:required]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = node<invalidating_value_source_definition>();

    const auto set_from_value = node([](i64 value, Out<TSS<i64>> output) { output.insert(value); });

    const auto size = node([](In<TSS<i64>> input) { return static_cast<i64>(input.size()); });

    const auto graph = [&] { return size(set_from_value(source())); };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({1}));
}

TEST_CASE("a time series set accepts its exact non-canonical item type",
          "[evaluation][series:tss][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][transition:addition][transition:removal][binding:direct]") {
    const auto contains = node([](In<TSS<i64>> input) { return input.contains(7); });

    const auto graph = [&](wp<TSS<i64>> input) { return contains(input); };

    const auto actual = testing::test_graph(graph, {{7}, {literal::REMOVED(7)}});

    CHECK(actual.equals({true, false}));
}

TEST_CASE("Out<TSS>::for_each supports erasing visited members across index words",
          "[evaluation][feature:node-api][series:tss][transition:removal][transition:update]") {
    const auto update = bg::node([](bg::In<bg::TS<bg::i64>> trigger, bg::Out<bg::TSS<bg::i64>> output) {
        auto visited = std::size_t{};
        output.for_each([&](const auto &key) {
            ++visited;
            output.erase(key);
        });
        CHECK(visited == (*trigger == 1 ? 130 : 0));

        if (*trigger == 0) {
            for (auto key = bg::i64{}; key < 130; ++key) {
                output.insert(key);
            }
        }
    });
    const auto size = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input.size()));
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> trigger) { return size(update(trigger)); };

    CHECK(bg::testing::test_graph(graph, {0, 1, 2}).equals<bg::i64>({130, 0, std::nullopt}));
}

namespace {

struct set_reference_condition_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct delayed_set_reference_condition_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const { output.set(1); }
};

const auto select_set_reference =
    node([](i64 condition,
            In<REF<TSS<i64>>, passive, optional> first,
            In<REF<TSS<i64>>, passive, optional> second,
            Out<REF<TSS<i64>>> output) { output.copy(condition == 0 ? first : second); });

struct invalidating_set_reference_definition {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TSS<i64>>, passive, optional> target;
    Out<REF<TSS<i64>>> output;

    void start(EvaluationContext *context) const {
        output.copy(target);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

} // namespace

TEST_CASE("a TSS input reports removed items when its reference selects an invalid target",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto invalid = node([](Out<TSS<i64>>) {});

    const auto condition = node<set_reference_condition_definition>();

    auto validity = std::vector<bool>{};
    auto sizes = std::vector<std::size_t>{};
    auto added = std::vector<std::vector<i64>>{};
    auto removed = std::vector<std::vector<i64>>{};
    auto was_removed = std::vector<bool>{};
    auto removed_cursor = std::vector<std::vector<i64>>{};
    const auto observe = node([&](In<TSS<i64>, optional> input) {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_items = std::vector<i64>{};
        input.for_each_added([&](const auto &item) { added_items.push_back(item.value()); });
        added.push_back(std::move(added_items));

        auto removed_items = std::vector<i64>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
        const auto removed_key = i64{11};
        was_removed.push_back(input.was_removed(Scalar<i64>::from(removed_key)));

        auto cursor_items = std::vector<i64>{};
        for (auto id = input.next_removed_id(0); id != util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            cursor_items.push_back(input.item_at(id).value());
        }
        removed_cursor.push_back(std::move(cursor_items));
    });

    const auto wiring = graph("name"_n = "literal_set_input", [&](wp<TSS<i64>> valid) {
        observe(select_set_reference(condition(), valid, invalid()));
    });

    (void)testing::test_graph(wiring, {{11, 12}});

    CHECK(validity == std::vector{true, false});
    CHECK(sizes == std::vector<std::size_t>{2, 0});
    CHECK(added == std::vector<std::vector<i64>>{{11, 12}, {}});
    CHECK(removed == std::vector<std::vector<i64>>{{}, {11, 12}});
    CHECK(was_removed == std::vector{false, true});
    CHECK(removed_cursor == std::vector<std::vector<i64>>{{}, {11, 12}});
}

TEST_CASE("a TSS input reports item changes when its reference selects another valid target",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:addition][transition:removal][binding:reference-to-value]"
          "[scalar:integer]") {
    const auto condition = node<set_reference_condition_definition>();

    auto validity = std::vector<bool>{};
    auto sizes = std::vector<std::size_t>{};
    auto added = std::vector<std::vector<i64>>{};
    auto removed = std::vector<std::vector<i64>>{};
    const auto observe = node([&](In<TSS<i64>, optional> input) {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_items = std::vector<i64>{};
        input.for_each_added([&](const auto &item) { added_items.push_back(item.value()); });
        added.push_back(std::move(added_items));

        auto removed_items = std::vector<i64>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
    });

    const auto wiring = graph("name"_n = "literal_set_inputs",
                              [&](wp<TSS<i64>> first, wp<TSS<i64>> second) {
                                  observe(select_set_reference(condition(), first, second));
                              });

    (void)testing::test_graph(wiring, {{11, 12}}, {{12, 13}});

    CHECK(validity == std::vector{true, true});
    CHECK(sizes == std::vector<std::size_t>{2, 2});
    CHECK(added == std::vector<std::vector<i64>>{{11, 12}, {13}});
    CHECK(removed == std::vector<std::vector<i64>>{{}, {11}});
}

TEST_CASE("a sampled TSS reports item ID exchanges across a reference rebind",
          "[evaluation][reference][series:tss][feature:sampling][feature:item-id-remapping]"
          "[binding:reference-to-value][transition:update]") {
    const auto condition = node<set_reference_condition_definition>();

    auto remappings_match_keys = std::vector<bool>{};
    auto remapping_found = std::vector<bool>{};
    auto remapping_counts = std::vector<std::size_t>{};
    const auto inspect = node([&](i64 phase, In<TSS<i64>> input, In<TSS<i64>> first) {
        auto actual = std::vector<std::pair<item_id_t, item_id_t>>{};
        input.for_each_item_id_remapping([&](item_id_t old_id, item_id_t new_id) {
            actual.emplace_back(old_id, new_id);
        });

        auto expected = std::vector<std::pair<item_id_t, item_id_t>>{};
        if (phase == 1) {
            for (auto new_id = input.next_item_id(0); new_id != util::occupancy_index::npos;
                 new_id = input.next_item_id(new_id + 1)) {
                const auto key = input.item_at(new_id);
                for (auto old_id = first.next_item_id(0); old_id != util::occupancy_index::npos;
                     old_id = first.next_item_id(old_id + 1)) {
                    if (first.item_at(old_id).value() == key.value() && old_id != new_id) {
                        expected.emplace_back(old_id, new_id);
                    }
                }
            }
        }

        std::ranges::sort(actual);
        std::ranges::sort(expected);
        remappings_match_keys.push_back(actual == expected);
        remapping_found.push_back(input.has_item_id_remapping());
        remapping_counts.push_back(actual.size());
    });

    const auto wiring = graph("name"_n = "literal_set_inputs", [&](wp<TSS<i64>> first, wp<TSS<i64>> second) {
        const auto phase = condition();
        inspect(phase, select_set_reference(phase, first, second), first);
    });

    (void)testing::test_graph(wiring, {{11, 12}}, {{12, 11}});

    CHECK(remappings_match_keys == std::vector{true, true});
    CHECK(remapping_found == std::vector{false, true});
    CHECK(remapping_counts == std::vector<std::size_t>{0, 2});
}

TEST_CASE("a sampled TSS does not report remapping for additions and removals at unchanged IDs",
          "[evaluation][reference][series:tss][feature:sampling][feature:item-id-remapping]"
          "[binding:reference-to-value][transition:addition][transition:removal]") {
    const auto condition = node<set_reference_condition_definition>();

    auto remapping_found = std::vector<bool>{};
    auto remapping_counts = std::vector<std::size_t>{};
    const auto inspect = node([&](In<TSS<i64>> input) {
        auto count = std::size_t{};
        input.for_each_item_id_remapping([&](item_id_t, item_id_t) { ++count; });
        remapping_found.push_back(input.has_item_id_remapping());
        remapping_counts.push_back(count);
    });

    const auto wiring = graph("name"_n = "literal_set_inputs", [&](wp<TSS<i64>> first, wp<TSS<i64>> second) {
        const auto phase = condition();
        inspect(select_set_reference(phase, first, second));
    });

    (void)testing::test_graph(wiring, {{11, 12}}, {{11, 13}});

    CHECK(remapping_found == std::vector{false, false});
    CHECK(remapping_counts == std::vector<std::size_t>{0, 0});
}

TEST_CASE("a TSS reports no item ID remapping for ordinary additions and removals",
          "[evaluation][series:tss][feature:item-id-remapping]"
          "[transition:addition][transition:removal]") {
    auto remapping_found = std::vector<bool>{};
    auto remapping_counts = std::vector<std::size_t>{};
    const auto inspect = node([&](In<TSS<i64>> input) {
        auto remapping_count = std::size_t{};
        input.for_each_item_id_remapping([&](item_id_t, item_id_t) { ++remapping_count; });
        remapping_found.push_back(input.has_item_id_remapping());
        remapping_counts.push_back(remapping_count);
    });

    const auto wiring = graph("name"_n = "literal_set_input", [&](wp<TSS<i64>> input) { inspect(input); });

    (void)testing::test_graph(wiring, {{10}, {literal::REMOVED(10), 20}});

    CHECK(remapping_found == std::vector{false, false});
    CHECK(remapping_counts == std::vector<std::size_t>{0, 0});
}

TEST_CASE("a TSS input does not report stale removals after an invalid target is rewired",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    struct observation {
        bool valid;
        i64 size;
        bool key_was_removed;

        bool operator==(const observation &) const = default;
    };

    const auto valid = node<clearing_referenced_set_source_definition>();

    const auto invalid = node([](Out<TSS<i64>>) {});

    const auto condition = node<delayed_set_reference_condition_definition>();

    auto removed_keys = std::vector<std::vector<i64>>{};
    const auto observe = node([&](In<TSS<i64>, optional> input) {
        auto removed = std::vector<i64>{};
        input.for_each_removed([&](const auto &item) { removed.push_back(item.value()); });

        const auto removed_key = i64{11};
        removed_keys.push_back(std::move(removed));
        return observation{input.valid(), static_cast<i64>(input.size()),
                           input.was_removed(Scalar<i64>::from(removed_key))};
    });

    const auto wiring = [&] { return observe(select_set_reference(condition(), valid(), invalid())); };
    const auto actual = testing::test_graph(wiring);

    CHECK(actual.equals<observation>({observation{true, 2, false}, observation{true, 0, true},
                                      observation{false, 0, false}}));
    CHECK(removed_keys == std::vector<std::vector<i64>>{{}, {11, 12}, {}});
}

TEST_CASE("a TSS reference transition reports removals when the old target clears in the retargeting cycle",
          "[series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto valid = node<clearing_referenced_set_source_definition>();

    const auto invalid = node([](Out<TSS<i64>>) {});

    const auto condition = node<set_reference_condition_definition>();

    auto validity = std::vector<bool>{};
    auto sizes = std::vector<std::size_t>{};
    auto added = std::vector<std::vector<i64>>{};
    auto removed = std::vector<std::vector<i64>>{};
    auto was_removed = std::vector<bool>{};
    const auto observe = node([&](In<TSS<i64>, optional> input) {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_items = std::vector<i64>{};
        input.for_each_added([&](const auto &item) { added_items.push_back(item.value()); });
        added.push_back(std::move(added_items));

        auto removed_items = std::vector<i64>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
        const auto removed_key = i64{11};
        was_removed.push_back(input.was_removed(Scalar<i64>::from(removed_key)));
    });

    const auto wiring = [&] { observe(select_set_reference(condition(), valid(), invalid())); };

    run_graph(wiring);

    CHECK(validity == std::vector{true, false});
    CHECK(sizes == std::vector<std::size_t>{2, 0});
    CHECK(added == std::vector<std::vector<i64>>{{11, 12}, {}});
    CHECK(removed == std::vector<std::vector<i64>>{{}, {11, 12}});
    CHECK(was_removed == std::vector{false, true});
}

TEST_CASE("a TSS input reports removed items when its reference becomes null",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]") {
    const auto reference = node<invalidating_set_reference_definition>();
    auto validity = std::vector<bool>{};
    auto sizes = std::vector<std::size_t>{};
    auto removed = std::vector<std::vector<i64>>{};
    auto was_removed = std::vector<bool>{};
    auto removed_cursor = std::vector<std::vector<i64>>{};
    const auto observe = node([&](In<TSS<i64>, optional> input) {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto removed_items = std::vector<i64>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
        const auto removed_key = i64{11};
        was_removed.push_back(input.was_removed(Scalar<i64>::from(removed_key)));

        auto cursor_items = std::vector<i64>{};
        for (auto id = input.next_removed_id(0); id != util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            cursor_items.push_back(input.item_at(id).value());
        }
        removed_cursor.push_back(std::move(cursor_items));
    });

    const auto wiring = graph("name"_n = "literal_set_input",
                              [&](wp<TSS<i64>> source) { observe(reference(source)); });

    (void)testing::test_graph(wiring, {{11, 12}});

    CHECK(validity == std::vector{true, false});
    CHECK(sizes == std::vector<std::size_t>{2, 0});
    CHECK(removed == std::vector<std::vector<i64>>{{}, {11, 12}});
    CHECK(was_removed == std::vector{false, true});
    CHECK(removed_cursor == std::vector<std::vector<i64>>{{}, {11, 12}});
}

TEST_CASE("a passive TSS input does not retain a stale removal sample",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][activity:passive][transition:removal][binding:reference-to-value]"
          "[scalar:integer]") {
    const auto reference = node<invalidating_set_reference_definition>();

    const auto trigger = node<delayed_set_reference_condition_definition>();
    auto validity = std::vector<bool>{};
    auto sizes = std::vector<std::size_t>{};
    auto removed = std::vector<std::vector<i64>>{};
    const auto observe = node([&](In<TSS<i64>, passive, optional> input, i64) {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto removed_items = std::vector<i64>{};
        input.for_each_removed([&](const auto &item) { removed_items.push_back(item.value()); });
        removed.push_back(std::move(removed_items));
    });

    const auto wiring = graph("name"_n = "literal_set_input", [&](wp<TSS<i64>> source) {
        observe(reference(source), trigger());
    });

    (void)testing::test_graph(wiring, {{11, 12}});

    CHECK(validity == std::vector{true, false});
    CHECK(sizes == std::vector<std::size_t>{2, 0});
    CHECK(removed == std::vector<std::vector<i64>>{{}, {}});
}

TEST_CASE("a generic input visitor sees TSS removals on reference invalidation",
          "[evaluation][reference][series:tss][feature:reference][feature:collection-change]"
          "[validity:optional][transition:removal][binding:reference-to-value][scalar:integer]"
          "[declaration:generic]") {
    const auto reference = node<invalidating_set_reference_definition>();
    auto validity = std::vector<bool>{};
    auto sizes = std::vector<std::size_t>{};
    auto removed_counts = std::vector<std::size_t>{};
    const auto observe = node([&](In<TS_T, optional> input) {
        input.visit(
            [&](const In<TSS<SCALAR>> *set) {
                validity.push_back(set->valid());
                sizes.push_back(set->size());
                auto removed_count = std::size_t{};
                set->for_each_removed([&](const auto &) { ++removed_count; });
                removed_counts.push_back(removed_count);
            },
            ddv::noop);
    });

    const auto wiring = graph("name"_n = "literal_set_input",
                              [&](wp<TSS<i64>> source) { observe(reference(source)); });

    (void)testing::test_graph(wiring, {{11, 12}});

    CHECK(validity == std::vector{true, false});
    CHECK(sizes == std::vector<std::size_t>{2, 0});
    CHECK(removed_counts == std::vector<std::size_t>{0, 2});
}

TEST_CASE("a TSS update rejects a root INVALID marker",
          "[evaluation][series:tss][feature:collection-change][validity]") {
    const auto source = node([](Out<TSS<i64>> output) { output.apply(literal::INVALID); });

    CHECK_THROWS_AS(run_graph([&] { source(); }), std::invalid_argument);
}
