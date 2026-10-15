#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

TEST_CASE("all_valid on a valid_items dictionary input requires only a valid dictionary",
          "[evaluation][tsd][feature:validity][series:tsd][validity:all-valid]"
          "[input-guarantee:valid-items][transition:invalidation][binding:direct]") {
    const auto count = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items, bg::all_valid> input,
                                   bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input.size()));
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return count(input); };

    const auto actual = bg::testing::test_graph(graph, {{{1, 10}, {2, 20}}, {{1, literal::INVALID}}});

    CHECK(actual.equals<bg::i64>({2, 1}));
}

TEST_CASE("valid_items survives erased input conversion",
          "[evaluation][tsd][feature:validity][input-guarantee:valid-items][feature:time-series-view]"
          "[feature:node-api]") {
    static_assert(sizeof(bg::TimeSeriesInputData) == 3 * sizeof(void *));

    const auto size_after_as = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                           bg::Out<bg::TS<bg::i64>> output) {
        const auto erased = bg::In<bg::TS_T>{input.input_data()};
        const auto dictionary = erased.as<bg::TSD<bg::i64, bg::TS<bg::i64>>>();
        const auto visible_size = dictionary.size();
        const auto physical_size = dictionary.runtime_type().item_count(dictionary.data());
        const auto flags_survived = erased.modified() && erased.input_data().valid_items() && dictionary.modified() &&
                                    dictionary.input_data().valid_items() && erased.all_valid() &&
                                    erased.all_valid(bg::TSD<bg::i64, bg::TS<bg::i64>>::type());
        output.set(flags_survived ? static_cast<bg::i64>(visible_size * 10 + physical_size) : -1);
    });
    const auto size_after_visit = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                              bg::Out<bg::TS<bg::i64>> output) {
        auto size = bg::i64{-1};
        input.visit([&](const bg::In<bg::TSD<bg::SCALAR, bg::TS_T>> *dictionary) {
            if (dictionary->modified() && dictionary->input_data().valid_items() && dictionary->all_valid()) {
                size = static_cast<bg::i64>(dictionary->size());
            }
        }, ddv::noop);
        output.set(size);
    });
    const auto runtime_copy = bg::node([](bg::In<bg::TS_T, bg::passive>,
                                          bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                          bg::Out<bg::TS_T> output) {
        output.copy(bg::In<bg::TS_T>{input.input_data()});
    });
    const auto size_and_second_key = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                                 bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input.size() * 10 + input.contains(2)));
    });

    SECTION("as") {
        const auto actual = bg::testing::test_graph(
            [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return size_after_as(input); },
            {{{1, 10}, {2, literal::INVALID}}});
        CHECK(actual.equals<bg::i64>({12}));
    }

    SECTION("visit") {
        const auto actual = bg::testing::test_graph(
            [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return size_after_visit(input); },
            {{{1, 10}, {2, literal::INVALID}}});
        CHECK(actual.equals<bg::i64>({1}));
    }

    SECTION("runtime copy") {
        const auto actual = bg::testing::test_graph(
            [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
                return size_and_second_key(runtime_copy(input, input));
            },
            {{{1, 10}, {2, literal::INVALID}}});
        CHECK(actual.equals<bg::i64>({10}));
    }
}

TEST_CASE("a valid_items dictionary input filters items and classifies validity changes",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {
    const auto sum = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                 bg::Out<bg::TS<bg::i64>> output) {
        auto result = bg::i64{};
        input.for_each_value([&](const auto &value) { result += *value; });
        output.set(result);
    });
    const auto size = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                  bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input.size()));
    });
    const auto key_values = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                        bg::Out<bg::TS<bg::i64>> output) {
        auto result = bg::i64{};
        input.for_each_key_value([&](const auto &key, const auto &value) { result += *key * 100 + *value; });
        output.set(result);
    });
    const auto selected = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                      bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.contains(2) ? *input[2] : -1);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> dictionary) {
        return sum(dictionary);
    };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    const auto visible_sizes = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return size(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto visible_key_values = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return key_values(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto selected_valid_value = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return selected(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    const auto contains_second = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                              bg::Out<bg::TS<bool>> output) { output.set(input.contains(2)); });
    const auto second_key_visible = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return contains_second(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    const auto added = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                   bg::Out<bg::TS<bg::i64>> output) {
        auto result = bg::i64{};
        input.for_each_added([&](const auto &key) { result += *key; });
        output.set(result);
    });
    const auto removed = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto result = bg::i64{};
        input.for_each_removed([&](const auto &key) { result += *key; });
        output.set(result);
    });
    const auto modified = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                      bg::Out<bg::TS<bg::i64>> output) {
        auto result = bg::i64{};
        input.for_each_modified([&](const auto &key) { result += *key; });
        output.set(result);
    });
    const auto added_keys = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return added(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto removed_keys = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return removed(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto modified_keys = bg::testing::test_graph(
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return modified(input); },
        {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    CHECK(actual.equals<bg::i64>({10, 31, 21}));
    CHECK(visible_sizes.equals<bg::i64>({1, 2, 1}));
    CHECK(visible_key_values.equals<bg::i64>({110, 331, 221}));
    CHECK(selected_valid_value.equals<bg::i64>({-1, 20, 21}));
    CHECK(second_key_visible.equals({false, true, true}));
    CHECK(added_keys.equals<bg::i64>({1, 2, 0}));
    CHECK(removed_keys.equals<bg::i64>({0, 0, 1}));
    CHECK(modified_keys.equals<bg::i64>({0, 1, 2}));
}

TEST_CASE("valid_items reports removal of valid keys but hides removed invalid keys",
          "[evaluation][tsd][feature:validity][series:tsd][transition:removal]"
          "[input-guarantee:valid-items][binding:direct]") {
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed = bg::i64{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(removed);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return inspect(input); };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, 10}, {2, literal::INVALID}}, {{1, literal::REMOVE}, {2, literal::REMOVE}}});

    CHECK(actual.equals<bg::i64>({0, 1}));
}

TEST_CASE("valid_items does not report a second removal when an invalid item is erased",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:direct]") {
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed = bg::i64{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(removed);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return inspect(input); };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, 10}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals<bg::i64>({0, 1, 0}));
}

TEST_CASE("valid_items does not repeat sampled invalidation when an invalid item is later erased",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value]") {
    const auto forward_reference = bg::node([](bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> input,
                                               bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        output.copy(input);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed = bg::i64{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(removed);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        return inspect(forward_reference(input));
    };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, 10}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals<bg::i64>({0, 1, 0}));
}

TEST_CASE("valid_items exposes filtered change cursors and key lookups",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {
    const auto sum_added = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                       bg::Out<bg::TS<bg::i64>> output) {
        auto added = bg::i64{};
        for (auto id = input.next_added_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_added_id(id + 1)) {
            added += *input.key_at(id);
        }
        output.set(added);
    });
    const auto sum_removed = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                         bg::Out<bg::TS<bg::i64>> output) {
        auto removed = bg::i64{};
        for (auto id = input.next_removed_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            removed += *input.key_at(id);
        }
        output.set(removed);
    });
    const auto sum_modified = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                          bg::Out<bg::TS<bg::i64>> output) {
        auto modified = bg::i64{};
        for (auto id = input.next_modified_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_modified_id(id + 1)) {
            modified += *input.key_at(id);
        }
        output.set(modified);
    });
    const auto added_graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return sum_added(input); };
    const auto removed_graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return sum_removed(input); };
    const auto modified_graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return sum_modified(input); };

    const auto added_keys = bg::testing::test_graph(
        added_graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto removed_keys = bg::testing::test_graph(
        removed_graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});
    const auto modified_keys = bg::testing::test_graph(
        modified_graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}});

    CHECK(added_keys.equals<bg::i64>({1, 2, 0}));
    CHECK(removed_keys.equals<bg::i64>({0, 0, 1}));
    CHECK(modified_keys.equals<bg::i64>({0, 1, 2}));
}

TEST_CASE("valid_items key lookups classify revalidation, invalidation, and item updates",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::In<bg::TS<bg::i64>> command, bg::Out<bg::TS<bool>> output) {
        const auto key_one_value = bg::i64{1};
        const auto key_two_value = bg::i64{2};
        const auto key_one = bg::Scalar<bg::i64>::from(key_one_value);
        const auto key_two = bg::Scalar<bg::i64>::from(key_two_value);
        auto result = false;
        if (*command == 1) {
            result = input.find_added_id(key_one) != bg::util::occupancy_index::npos;
        } else if (*command == 2) {
            result = input.find_added_id(key_two) != bg::util::occupancy_index::npos && input.item_modified(key_one);
        } else {
            result = input.find_removed_id(key_one) != bg::util::occupancy_index::npos &&
                     input.find_modified_id(key_two) != bg::util::occupancy_index::npos;
        }
        output.set(result);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                           bg::wp<bg::TS<bg::i64>> command) { return inspect(input, command); };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}}, {{1, literal::INVALID}, {2, 21}}},
        {1, 2, 3});

    CHECK(actual.equals({true, true, true}));
}

TEST_CASE("valid_items hides invalid items from keyed lookup",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation]"
          "[input-guarantee:valid-items][binding:direct]") {
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bool>> output) {
        auto keyed_lookup_is_hidden = false;
        try {
            static_cast<void>(input[2]);
        } catch (const std::out_of_range &) {
            keyed_lookup_is_hidden = true;
        }
        output.set(!input.contains(2) && keyed_lookup_is_hidden);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        return inspect(input);
    };

    const auto actual = bg::testing::test_graph(graph, {{{1, 10}, {2, literal::INVALID}}});

    CHECK(actual.equals({true}));
}

TEST_CASE("valid_items retains removed cursor IDs in a sampled dictionary",
          "[evaluation][tsd][feature:validity][series:tsd][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value]") {
    const auto select = bg::node([](bg::In<bg::TS<bool>> choose_second,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed_key_and_validity = bg::i64{};
        for (auto id = input.next_removed_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            removed_key_and_validity += *input.key_at(id) * 10 + input.item_at(id).valid();
        }
        output.set(removed_key_and_validity);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> choose_second,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second) {
        return inspect(select(choose_second, first, second));
    };

    const auto actual = bg::testing::test_graph(
        graph, {false, true}, {{{1, 10}, {2, literal::INVALID}}}, {{{3, 30}}});

    CHECK(actual.equals<bg::i64>({0, 10}));
}

TEST_CASE("valid_items cursors include a sampled removal and invalidation sharing an item ID",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value][shape:multiple]") {
    const auto select = bg::node([](bg::In<bg::TS<bool>> choose_second,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed_count = bg::i64{};
        auto removed_key_sum = bg::i64{};
        input.for_each_removed([&](const auto &key) {
            ++removed_count;
            removed_key_sum += *key;
        });
        output.set(removed_count * 10 + removed_key_sum);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> choose_second,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second) {
        return inspect(select(choose_second, first, second));
    };

    const auto actual = bg::testing::test_graph(graph, {false, true}, {{{1, 10}, {2, 20}}},
                                                {{{2, literal::INVALID}}});

    CHECK(actual.equals<bg::i64>({0, 23}));
}
