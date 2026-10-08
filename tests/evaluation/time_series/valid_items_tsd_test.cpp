#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {

struct partially_valid_dictionary_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start(bg::EvaluationContext *context) const {
        output[1].set(10);
        output[2].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[1].invalidate(); }
};

} // namespace

TEST_CASE("all_valid on a valid_items dictionary input requires only a valid dictionary",
          "[evaluation][tsd][feature:validity][series:tsd][validity:all-valid]"
          "[input-guarantee:valid-items][transition:invalidation][binding:direct]") {
    const auto source = bg::node<partially_valid_dictionary_source_definition>();
    const auto count = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items, bg::all_valid> input,
                                   bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<std::int64_t>(input.size()));
    });
    const auto graph = [&] { return count(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<bg::i64>({2, 1}));
}

namespace {

struct valid_item_dictionary_source_definition {
    bg::In<bg::TS<bg::i64>> command;
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void evaluate() const {
        if (*command == 1) {
            output[1].set(10);
            output[2].invalidate();
        } else if (*command == 2) {
            output[1].set(11);
            output[2].set(20);
        } else {
            output[1].invalidate();
            output[2].set(21);
        }
    }
};

struct valid_item_dictionary_removal_source_definition {
    bg::In<bg::TS<bg::i64>> command;
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const {
        output[1].set(10);
        output[2].invalidate();
    }

    void evaluate() const {
        if (*command == 2) {
            output.erase(1);
            output.erase(2);
        }
    }
};

struct invalid_then_removed_dictionary_source_definition {
    bg::In<bg::TS<bg::i64>> command;
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const { output[1].set(10); }

    void evaluate() const {
        if (*command == 2) {
            output[1].invalidate();
        } else if (*command == 3) {
            output.erase(1);
        }
    }
};

struct dictionary_with_invalid_item_source_definition {
    bg::In<bg::TS<bg::i64>> value;
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const {
        output[1].set(*value);
        output[2].invalidate();
    }
};

struct valid_item_dictionary_reference_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const {
        output[1].set(10);
        output[2].invalidate();
    }
};

struct second_valid_item_dictionary_reference_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const { output[3].set(30); }
};

struct two_key_dictionary_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const {
        output[1].set(10);
        output[2].set(20);
    }
};

struct one_invalid_key_dictionary_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::i64>>> output;

    void start() const { output[2].invalidate(); }
};

} // namespace

TEST_CASE("a valid_items dictionary input filters items and classifies validity changes",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {
    const auto source = bg::node<valid_item_dictionary_source_definition>();
    const auto sum = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                 bg::Out<bg::TS<bg::i64>> output) {
        auto result = std::int64_t{};
        input.for_each_value([&](const auto &value) { result += *value; });
        output.set(result);
    });
    const auto size = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                  bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<std::int64_t>(input.size()));
    });
    const auto key_values = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                        bg::Out<bg::TS<bg::i64>> output) {
        auto result = std::int64_t{};
        input.for_each_key_value([&](const auto &key, const auto &value) { result += *key * 100 + *value; });
        output.set(result);
    });
    const auto selected = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                      bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.contains(2) ? *input[2] : -1);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command) {
        const auto dictionary = source(command);
        return sum(dictionary);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    const auto visible_sizes = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return size(source(command)); }, std::array{1, 2, 3});
    const auto visible_key_values = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return key_values(source(command)); }, std::array{1, 2, 3});
    const auto selected_valid_value = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return selected(source(command)); }, std::array{1, 2, 3});

    const auto contains_second = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                              bg::Out<bg::TS<bool>> output) { output.set(input.contains(2)); });
    const auto second_key_visible = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return contains_second(source(command)); }, std::array{1, 2, 3});

    const auto added = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                   bg::Out<bg::TS<bg::i64>> output) {
        auto result = std::int64_t{};
        input.for_each_added([&](const auto &key) { result += *key; });
        output.set(result);
    });
    const auto removed = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto result = std::int64_t{};
        input.for_each_removed([&](const auto &key) { result += *key; });
        output.set(result);
    });
    const auto modified = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                      bg::Out<bg::TS<bg::i64>> output) {
        auto result = std::int64_t{};
        input.for_each_modified([&](const auto &key) { result += *key; });
        output.set(result);
    });
    const auto added_keys = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return added(source(command)); }, std::array{1, 2, 3});
    const auto removed_keys = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return removed(source(command)); }, std::array{1, 2, 3});
    const auto modified_keys = bg::testing::test_graph(
        [&](bg::wp<bg::TS<bg::i64>> command) { return modified(source(command)); }, std::array{1, 2, 3});

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
    const auto source = bg::node<valid_item_dictionary_removal_source_definition>();
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed = std::int64_t{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(removed);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command) { return inspect(source(command)); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals<bg::i64>({0, 1}));
}

TEST_CASE("valid_items does not report a second removal when an invalid item is erased",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:direct]") {
    const auto source = bg::node<invalid_then_removed_dictionary_source_definition>();
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed = std::int64_t{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(removed);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command) { return inspect(source(command)); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals<bg::i64>({0, 1, 0}));
}

TEST_CASE("valid_items does not repeat sampled invalidation when an invalid item is later erased",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value]") {
    const auto source = bg::node<invalid_then_removed_dictionary_source_definition>();
    const auto forward_reference = bg::node([](bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> input,
                                               bg::Out<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> output) {
        output.copy(input);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed = std::int64_t{};
        input.for_each_removed([&](const auto &key) { removed += *key; });
        output.set(removed);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command) {
        return inspect(forward_reference(source(command)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals<bg::i64>({0, 1, 0}));
}

TEST_CASE("valid_items exposes filtered change cursors and key lookups",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {
    const auto source = bg::node<valid_item_dictionary_source_definition>();
    const auto sum_added = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                       bg::Out<bg::TS<bg::i64>> output) {
        auto added = std::int64_t{};
        for (auto id = input.next_added_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_added_id(id + 1)) {
            added += *input.key_at(id);
        }
        output.set(added);
    });
    const auto sum_removed = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                         bg::Out<bg::TS<bg::i64>> output) {
        auto removed = std::int64_t{};
        for (auto id = input.next_removed_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            removed += *input.key_at(id);
        }
        output.set(removed);
    });
    const auto sum_modified = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                          bg::Out<bg::TS<bg::i64>> output) {
        auto modified = std::int64_t{};
        for (auto id = input.next_modified_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_modified_id(id + 1)) {
            modified += *input.key_at(id);
        }
        output.set(modified);
    });
    const auto added_graph = [&](bg::wp<bg::TS<bg::i64>> command) { return sum_added(source(command)); };
    const auto removed_graph = [&](bg::wp<bg::TS<bg::i64>> command) { return sum_removed(source(command)); };
    const auto modified_graph = [&](bg::wp<bg::TS<bg::i64>> command) { return sum_modified(source(command)); };

    const auto added_keys = bg::testing::test_graph(added_graph, std::array{1, 2, 3});
    const auto removed_keys = bg::testing::test_graph(removed_graph, std::array{1, 2, 3});
    const auto modified_keys = bg::testing::test_graph(modified_graph, std::array{1, 2, 3});

    CHECK(added_keys.equals<bg::i64>({1, 2, 0}));
    CHECK(removed_keys.equals<bg::i64>({0, 0, 1}));
    CHECK(modified_keys.equals<bg::i64>({0, 1, 2}));
}

TEST_CASE("valid_items key lookups classify revalidation, invalidation, and item updates",
          "[evaluation][tsd][feature:validity][series:tsd][transition:addition][transition:update]"
          "[transition:invalidation][input-guarantee:valid-items][binding:direct]") {
    const auto source = bg::node<valid_item_dictionary_source_definition>();
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::In<bg::TS<bg::i64>> command, bg::Out<bg::TS<bool>> output) {
        const auto key_one_value = std::int64_t{1};
        const auto key_two_value = std::int64_t{2};
        const auto key_one = bg::Scalar<std::int64_t>::from(key_one_value);
        const auto key_two = bg::Scalar<std::int64_t>::from(key_two_value);
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
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> command) { return inspect(source(command), command); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({true, true, true}));
}

TEST_CASE("valid_items hides invalid items from keyed lookup",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation]"
          "[input-guarantee:valid-items][binding:direct]") {
    const auto source = bg::node<dictionary_with_invalid_item_source_definition>();
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bool>> output) {
        auto keyed_lookup_is_hidden = false;
        try {
            static_cast<void>(input[2]);
        } catch (const std::out_of_range &) {
            keyed_lookup_is_hidden = true;
        }
        output.set(!input.contains(2) && keyed_lookup_is_hidden);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> value) {
        return inspect(source(value));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10});

    CHECK(actual.equals({true}));
}

TEST_CASE("valid_items retains removed cursor IDs in a sampled dictionary",
          "[evaluation][tsd][feature:validity][series:tsd][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value]") {
    const auto first = bg::node<valid_item_dictionary_reference_source_definition>();
    const auto second = bg::node<second_valid_item_dictionary_reference_source_definition>();
    const auto select = bg::node([](bg::In<bg::TS<bool>> choose_second,
                                    bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed_key_and_validity = std::int64_t{};
        for (auto id = input.next_removed_id(0); id != bg::util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            removed_key_and_validity += *input.key_at(id) * 10 + input.item_at(id).valid();
        }
        output.set(removed_key_and_validity);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> choose_second) {
        return inspect(select(choose_second, first(), second()));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals<bg::i64>({0, 10}));
}

TEST_CASE("valid_items cursors include a sampled removal and invalidation sharing an item ID",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation][transition:removal]"
          "[input-guarantee:valid-items][binding:reference-to-value][shape:multiple]") {
    const auto first = bg::node<two_key_dictionary_source_definition>();
    const auto second = bg::node<one_invalid_key_dictionary_source_definition>();
    const auto select = bg::node([](bg::In<bg::TS<bool>> choose_second,
                                    bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<std::int64_t, bg::TS<bg::i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::i64>>, bg::valid_items> input,
                                     bg::Out<bg::TS<bg::i64>> output) {
        auto removed_count = std::int64_t{};
        auto removed_key_sum = std::int64_t{};
        input.for_each_removed([&](const auto &key) {
            ++removed_count;
            removed_key_sum += *key;
        });
        output.set(removed_count * 10 + removed_key_sum);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> choose_second) {
        return inspect(select(choose_second, first(), second()));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals<bg::i64>({0, 23}));
}

