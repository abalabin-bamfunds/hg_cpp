#include <bg/graph.hpp>
#include <bg/graph/context.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/nested.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace bg;

TEST_CASE("all_valid on a valid_items dictionary input requires only a valid dictionary",
          "[evaluation][tsd][feature:validity][series:tsd][validity:all-valid]"
          "[input-guarantee:valid-items][transition:invalidation][binding:direct]") {

    const auto count = node([](In<TSD<i64, TS<i64>>, valid_items, all_valid> input) {
        return static_cast<i64>(input.size());
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return count(input); };

    const auto actual = testing::test_graph(graph, {{{1, 10}, {2, 20}}, {{1, literal::INVALID}}});

    CHECK(actual.equals<i64>({2, 1}));
}

TEST_CASE("a nested graph preserves a fixed valid_items input",
          "[evaluation][feature:nested-graph][feature:validity][series:tsd][input-guarantee:valid-items]") {
    using namespace bg::literals;

    const auto count = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        return static_cast<i64>(input.size());
    });

    const auto inner = graph(
        "name"_n = "valid_items_inner",
        [&](wp<TSD<i64, TS<i64>>, valid_items> input) { return count(input); });

    const auto outer = [&](wp<TSD<i64, TS<i64>>> input, wp<TS<bool>> run) { return nested(inner, run, input); };

    const auto actual = testing::test_graph(
        outer, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}}, {true, true});

    CHECK(actual.equals<i64>({1, 2}));
}

TEST_CASE("a nested graph preserves valid_items and scalar fields in a captured TSB",
          "[evaluation][feature:nested-graph][feature:validity][feature:variadic-wiring][series:tsd][series:tsb]"
          "[input-guarantee:valid-items][representation:free]") {
    using namespace bg::literals;

    const auto count = node([](In<TSD<i64, TS<i64>>, valid_items> values, i64 offset) {
        return static_cast<i64>(values.size()) + offset;
    });

    const auto inner = graph([&](wp<TSB<SCHEMA>, "values"_n, var_kwargs> values) {
        return count(values["dictionary"], values["offset"]);
    });

    const auto outer = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> run) {
        const auto pack = bundle("dictionary"_n = valid_items(values), "offset"_n = i64{40});
        return nested(inner, run, "values"_n = pack);
    };

    const auto actual = testing::test_graph(
        outer, {{{1, 10}, {2, literal::INVALID}}, {{2, 20}}}, {true, true});

    CHECK(actual.equals<i64>({41, 42}));
}

TEST_CASE("a lexical context preserves valid_items on a TSD input",
          "[evaluation][feature:context-time-series][feature:validity][series:tsd][input-guarantee:valid-items]") {
    using namespace bg::literals;

    const auto count = node([](In<TSD<i64, TS<i64>>, "values"_n, context, valid_items> values) {
        return static_cast<i64>(values.size());
    });

    const auto contextual = [&](wp<TSD<i64, TS<i64>>> values) {
        return with_context("values"_n = valid_items(values), [&] { return count(); });
    };

    const auto actual = testing::test_graph(contextual, {{{1, 10}, {2, literal::INVALID}}, {{2, 20}}});

    CHECK(actual.equals<i64>({1, 2}));
}

TEST_CASE("a materialisation option preserves valid_items on a TSD input",
          "[evaluation][feature:wiring][feature:validity][feature:wiring-materialisation][series:tsd]"
          "[input-guarantee:valid-items]") {

    const auto count = node([](In<TSD<i64, TS<i64>>, valid_items> values) {
        return static_cast<i64>(values.size());
    });

    const auto materialized = [&](wp<TSD<i64, TS<i64>>> values) {
        auto deferred = WiringContext::current().wiring().create_wp<TSD<i64, TS<i64>>>(valid_items(values));
        deferred.materialisation_option<TSD<i64, TS<i64>>>(
            [](wp<TSD<i64, TS<i64>>, valid_items> input) { return input; }, 0.0);
        return count(valid_items(deferred));
    };

    const auto actual = testing::test_graph(materialized, {{{1, 10}, {2, literal::INVALID}}, {{2, 20}}});

    CHECK(actual.equals<i64>({1, 2}));
}

TEST_CASE("valid_items survives erased input conversion",
          "[evaluation][tsd][feature:validity][input-guarantee:valid-items][feature:time-series-view]"
          "[feature:node-api]") {
    static_assert(sizeof(TimeSeriesInputData) == 3 * sizeof(void *));

    const auto size_after_as = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        const auto erased = In<TS_T>{input.input_data()};
        const auto dictionary = erased.as<TSD<i64, TS<i64>>>();
        const auto visible_size = dictionary.size();
        const auto physical_size = dictionary.runtime_type().item_count(dictionary.data());
        const auto flags_survived = erased.modified() && erased.input_data().valid_items() && dictionary.modified() &&
                                    dictionary.input_data().valid_items() && erased.all_valid() &&
                                    erased.all_valid(TSD<i64, TS<i64>>::type());
        return flags_survived ? static_cast<i64>(visible_size * 10 + physical_size) : -1;
    });

    const auto size_after_visit = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto size = i64{-1};
        input.visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
                if (dictionary->modified() && dictionary->input_data().valid_items() && dictionary->all_valid()) {
                    size = static_cast<i64>(dictionary->size());
                }
            },
            ddv::noop);
        return size;
    });

    const auto runtime_copy = node(
        [](In<TS_T, passive>, In<TSD<i64, TS<i64>>, valid_items> input, Out<TS_T> output) {
            output.copy(In<TS_T>{input.input_data()});
        });

    const auto size_and_second_key = node([](In<TSD<i64, TS<i64>>> input) {
        return static_cast<i64>(input.size() * 10 + input.contains(2));
    });

    SECTION("as") {
        const auto actual = testing::test_graph(
            [&](wp<TSD<i64, TS<i64>>> input) { return size_after_as(input); },
            {{{1, 10}, {2, literal::INVALID}}});
        CHECK(actual.equals<i64>({12}));
    }

    SECTION("visit") {
        const auto actual = testing::test_graph(
            [&](wp<TSD<i64, TS<i64>>> input) { return size_after_visit(input); },
            {{{1, 10}, {2, literal::INVALID}}});
        CHECK(actual.equals<i64>({1}));
    }

    SECTION("runtime copy") {
        const auto actual = testing::test_graph(
            [&](wp<TSD<i64, TS<i64>>> input) {
                return size_and_second_key(runtime_copy(input, input));
            },
            {{{1, 10}, {2, literal::INVALID}}});
        CHECK(actual.equals<i64>({10}));
    }
}

TEST_CASE("a valid_items dictionary input filters items and classifies validity changes",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {

    const auto sum = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto result = i64{};
        input.for_each_value([&](const auto &value) { result += *value; });
        return result;
    });

    const auto size = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        return static_cast<i64>(input.size());
    });

    const auto key_values = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto result = i64{};
        input.for_each_key_value([&](const auto &key, const auto &value) { result += *key * 100 + *value; });
        return result;
    });

    const auto selected = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        return input.contains(2) ? *input[2] : -1;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> dictionary) {
        return sum(dictionary);
    };

    const auto actual = testing::test_graph(
        graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    const auto visible_sizes = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return size(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto visible_key_values = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return key_values(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto selected_valid_value = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return selected(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    const auto contains_second =
        node([](In<TSD<i64, TS<i64>>, valid_items> input) { return input.contains(2); });
    const auto second_key_visible = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return contains_second(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    const auto added = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto result = i64{};
        input.for_each_added([&](const auto &key) { result += *key; });
        return result;
    });

    const auto removed = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto result = i64{};
        input.for_each_removed([&](const auto &key) { result += *key; });
        return result;
    });

    const auto modified = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto result = i64{};
        input.for_each_modified([&](const auto &key) { result += *key; });
        return result;
    });

    const auto added_keys = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return added(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto removed_keys = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return removed(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto modified_keys = testing::test_graph(
        [&](wp<TSD<i64, TS<i64>>> input) { return modified(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    CHECK(actual.equals<i64>({10, 31, 21}));
    CHECK(visible_sizes.equals<i64>({1, 2, 1}));
    CHECK(visible_key_values.equals<i64>({110, 331, 221}));
    CHECK(selected_valid_value.equals<i64>({-1, 20, 21}));
    CHECK(second_key_visible.equals({false, true, true}));
    CHECK(added_keys.equals<i64>({1, 2, 0}));
    CHECK(removed_keys.equals<i64>({0, 0, 1}));
    CHECK(modified_keys.equals<i64>({0, 1, 2}));
}

TEST_CASE("valid_items reports removal of valid keys but hides removed invalid keys",
          "[evaluation][tsd][feature:validity][series:tsd][transition:removal]"
          "[input-guarantee:valid-items][binding:direct]") {

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto removed = i64{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        return removed;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); };

    const auto actual = testing::test_graph(
        graph, {{{1, 10}, {2, literal::INVALID}}, {{1, literal::REMOVE}, {2, literal::REMOVE}}});

    CHECK(actual.equals<i64>({0, 1}));
}

TEST_CASE("valid_items does not report a second removal when an invalid item is erased",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:direct]") {

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto removed = i64{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        return removed;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); };

    const auto actual = testing::test_graph(
        graph, {{{1, 10}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals<i64>({0, 1, 0}));
}

TEST_CASE("valid_items does not repeat sampled invalidation when an invalid item is later erased",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value]") {

    const auto forward_reference = node([](In<REF<TSD<i64, TS<i64>>>> input,
                                               Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(input);
    });

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto removed = i64{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        return removed;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) {
        return inspect(forward_reference(input));
    };

    const auto actual = testing::test_graph(
        graph, {{{1, 10}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals<i64>({0, 1, 0}));
}

TEST_CASE("valid_items exposes filtered change cursors and key lookups",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {

    const auto sum_added = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto added = i64{};
        for (auto id = input.next_added_id(0); id != util::occupancy_index::npos;
             id = input.next_added_id(id + 1)) {
            added += *input.key_at(id);
        }
        return added;
    });

    const auto sum_removed = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto removed = i64{};
        for (auto id = input.next_removed_id(0); id != util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            removed += *input.key_at(id);
        }
        return removed;
    });

    const auto sum_modified = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto modified = i64{};
        for (auto id = input.next_modified_id(0); id != util::occupancy_index::npos;
             id = input.next_modified_id(id + 1)) {
            modified += *input.key_at(id);
        }
        return modified;
    });

    const auto added_graph = [&](wp<TSD<i64, TS<i64>>> input) { return sum_added(input); };

    const auto removed_graph = [&](wp<TSD<i64, TS<i64>>> input) { return sum_removed(input); };

    const auto modified_graph = [&](wp<TSD<i64, TS<i64>>> input) { return sum_modified(input); };

    const auto added_keys = testing::test_graph(
        added_graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto removed_keys = testing::test_graph(
        removed_graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto modified_keys = testing::test_graph(
        modified_graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    CHECK(added_keys.equals<i64>({1, 2, 0}));
    CHECK(removed_keys.equals<i64>({0, 0, 1}));
    CHECK(modified_keys.equals<i64>({0, 1, 2}));
}

TEST_CASE("valid_items key lookups classify revalidation, invalidation, and item updates",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input, In<TS<i64>> command) {
        const auto key_one_value = i64{1};
        const auto key_two_value = i64{2};
        const auto key_one = Scalar<i64>::from(key_one_value);
        const auto key_two = Scalar<i64>::from(key_two_value);
        auto result = false;
        if (*command == 1) {
            result = input.find_added_id(key_one) != util::occupancy_index::npos;
        } else if (*command == 2) {
            result = input.find_added_id(key_two) != util::occupancy_index::npos && input.item_modified(key_one);
        } else {
            result = input.find_removed_id(key_one) != util::occupancy_index::npos &&
                     input.find_modified_id(key_two) != util::occupancy_index::npos;
        }
        return result;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input, wp<TS<i64>> command) { return inspect(input, command); };

    const auto actual = testing::test_graph(
        graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}},
        {1, 2, 3});

    CHECK(actual.equals({true, true, true}));
}

TEST_CASE("valid_items hides invalid items from keyed lookup",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation]"
          "[input-guarantee:valid-items][binding:direct]") {

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto keyed_lookup_is_hidden = false;
        try {
            static_cast<void>(input[2]);
        } catch (const std::out_of_range &) {
            keyed_lookup_is_hidden = true;
        }
        return !input.contains(2) && keyed_lookup_is_hidden;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); };

    const auto actual = testing::test_graph(graph, {{{1, 10}, {2, literal::INVALID}}});

    CHECK(actual.equals({true}));
}

TEST_CASE("valid_items retains removed cursor IDs in a sampled dictionary",
          "[evaluation][tsd][feature:validity][series:tsd][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value]") {

    const auto select = node([](In<TS<bool>> choose_second,
                                    In<REF<TSD<i64, TS<i64>>>> first,
                                    In<REF<TSD<i64, TS<i64>>>> second,
                                    Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto removed_key_and_validity = i64{};
        for (auto id = input.next_removed_id(0); id != util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            removed_key_and_validity += *input.key_at(id) * 10 + input.item_at(id).valid();
        }
        return removed_key_and_validity;
    });

    const auto graph = [&](wp<TS<bool>> choose_second,
                           wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        return inspect(select(choose_second, first, second));
    };

    const auto actual = testing::test_graph(
        graph, {false, true}, {{{1, 10}, {2, literal::INVALID}}}, {{{3, 30}}});

    CHECK(actual.equals<i64>({0, 10}));
}

TEST_CASE("valid_items cursors include a sampled removal and invalidation sharing an item ID",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value][shape:multiple]") {

    const auto select = node([](In<TS<bool>> choose_second,
                                    In<REF<TSD<i64, TS<i64>>>> first,
                                    In<REF<TSD<i64, TS<i64>>>> second,
                                    Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });

    const auto inspect = node([](In<TSD<i64, TS<i64>>, valid_items> input) {
        auto removed_count = i64{};
        auto removed_key_sum = i64{};
        input.for_each_removed([&](const auto &key) {
            ++removed_count;
            removed_key_sum += *key;
        });
        return removed_count * 10 + removed_key_sum;
    });

    const auto graph = [&](wp<TS<bool>> choose_second,
                           wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        return inspect(select(choose_second, first, second));
    };

    const auto actual = testing::test_graph(graph, {false, true}, {{{1, 10}, {2, 20}}}, {{{2, literal::INVALID}}});

    CHECK(actual.equals<i64>({0, 23}));
}
