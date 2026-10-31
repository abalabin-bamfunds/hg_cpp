#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

using namespace bg;

TEST_CASE("a dictionary graph preserves its input updates",
          "[evaluation][tsd][feature:collection-change][series:tsd][scalar:integer][transition:update]") {
    const auto identity = [](wp<TSD<i64, TS<i64>>> input) { return input; };

    const auto actual = bg::run_graph(identity, {{{1, 10}, {2, 20}}, {{1, 11}}});

    CHECK(actual.equals({{{1, 10}, {2, 20}}, {{1, 11}}}));
}

TEST_CASE("dictionary change selections distinguish updates from modifications",
          "[evaluation][tsd][feature:collection-change][series:tsd][transition:addition][transition:update]"
          "[transition:removal][scalar:integer][binding:direct]") {
    struct selection_counts {
        i64 keys;
        i64 values;
        i64 key_values;

        bool operator==(const selection_counts &) const = default;
    };

    struct cursor_counts {
        i64 updated;
        i64 modified;
        i64 removed;

        bool operator==(const cursor_counts &) const = default;
    };

    struct observation {
        selection_counts added;
        selection_counts updated;
        selection_counts modified;
        selection_counts removed;
        cursor_counts cursors;
        bool first_key_found_as_updated;
        bool first_key_found_as_modified;
        bool second_key_found_as_modified;
        bool first_key_updated;
        bool second_key_modified;

        bool operator==(const observation &) const = default;
    };

    const auto inspect = node([](In<TSD<i64, TS<i64>>> input) {
        auto result = observation{};
        input.for_each_added_key([&](const auto &) { ++result.added.keys; });
        input.for_each_updated_key([&](const auto &) { ++result.updated.keys; });
        input.for_each_modified_key([&](const auto &) { ++result.modified.keys; });
        input.for_each_removed_key([&](const auto &) { ++result.removed.keys; });
        input.for_each_added_value([&](const auto &) { ++result.added.values; });
        input.for_each_updated_value([&](const auto &) { ++result.updated.values; });
        input.for_each_modified_value([&](const auto &) { ++result.modified.values; });
        input.for_each_removed_value([&](const auto &) { ++result.removed.values; });
        input.for_each_added_key_value([&](const auto &, const auto &) { ++result.added.key_values; });
        input.for_each_updated_key_value([&](const auto &, const auto &) { ++result.updated.key_values; });
        input.for_each_modified_key_value([&](const auto &, const auto &) { ++result.modified.key_values; });
        input.for_each_removed_key_value([&](const auto &, const auto &) { ++result.removed.key_values; });

        for (auto id = input.next_updated_id(0); id != util::occupancy_index::npos;
             id = input.next_updated_id(id + 1)) {
            ++result.cursors.updated;
        }
        for (auto id = input.next_modified_id(0); id != util::occupancy_index::npos;
             id = input.next_modified_id(id + 1)) {
            ++result.cursors.modified;
        }
        for (auto id = input.next_removed_id(0); id != util::occupancy_index::npos;
             id = input.next_removed_id(id + 1)) {
            ++result.cursors.removed;
        }

        const auto first_key_value = i64{1};
        const auto second_key_value = i64{2};
        const auto first_key = Scalar<i64>::from(first_key_value);
        const auto second_key = Scalar<i64>::from(second_key_value);
        result.first_key_found_as_updated = input.find_updated_id(first_key) != util::occupancy_index::npos;
        result.first_key_found_as_modified = input.find_modified_id(first_key) != util::occupancy_index::npos;
        result.second_key_found_as_modified = input.find_modified_id(second_key) != util::occupancy_index::npos;
        result.first_key_updated = input.item_updated(first_key);
        result.second_key_modified = input.item_modified(second_key);
        return result;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); };
    const auto actual = bg::run_graph(
        graph, {{{1, literal::INVALID}}, {{1, 10}, {2, literal::INVALID}}, {{1, 11}, {2, 20}},
                {{1, literal::REMOVE}, {2, literal::REMOVE}}});

    CHECK(actual.equals<observation>({
        observation{{1, 1, 1}, {}, {1, 1, 1}, {}, {0, 1, 0}, false, true, false, false, false},
        observation{{1, 1, 1}, {1, 1, 1}, {2, 2, 2}, {}, {1, 2, 0}, true, true, true, true, true},
        observation{{}, {2, 2, 2}, {2, 2, 2}, {}, {2, 2, 0}, true, true, true, true, true},
        observation{{}, {}, {}, {2, 2, 2}, {0, 0, 2}, false, false, false, false, false},
    }));
}

TEST_CASE("dictionary updated selections exclude additions and modified selections include them",
          "[evaluation][tsd][feature:collection-change][series:tsd][transition:addition][transition:update]"
          "[scalar:integer][binding:direct]") {
    struct observation {
        i64 added_keys;
        i64 updated_keys;
        i64 modified_keys;
        i64 removed_keys;
        i64 added_values;
        i64 updated_values;
        i64 modified_values;
        i64 removed_values;
        i64 added_key_values;
        i64 updated_key_values;
        i64 modified_key_values;
        i64 removed_key_values;

        bool operator==(const observation &) const = default;
    };

    const auto inspect = node([](In<TSD<i64, TS<i64>>> input) {
        auto result = observation{};
        input.for_each_added_key([&](const auto &key) { result.added_keys += *key; });
        input.for_each_updated_key([&](const auto &key) { result.updated_keys += *key; });
        input.for_each_modified_key([&](const auto &key) { result.modified_keys += *key; });
        input.for_each_removed_key([&](const auto &key) { result.removed_keys += *key; });
        input.for_each_added_value([&](const auto &value) { result.added_values += value.valid() ? *value : 0; });
        input.for_each_updated_value([&](const auto &value) { result.updated_values += value.valid() ? *value : 0; });
        input.for_each_modified_value([&](const auto &value) { result.modified_values += value.valid() ? *value : 0; });
        input.for_each_removed_value([&](const auto &value) { result.removed_values += value.valid() ? *value : 0; });
        input.for_each_added_key_value([&](const auto &key, const auto &value) {
            result.added_key_values += *key + (value.valid() ? *value : 0);
        });
        input.for_each_updated_key_value([&](const auto &key, const auto &value) {
            result.updated_key_values += *key + (value.valid() ? *value : 0);
        });
        input.for_each_modified_key_value([&](const auto &key, const auto &value) {
            result.modified_key_values += *key + (value.valid() ? *value : 0);
        });
        input.for_each_removed_key_value([&](const auto &key, const auto &value) {
            result.removed_key_values += *key + (value.valid() ? *value : 0);
        });
        return result;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); };
    const auto actual = bg::run_graph(
        graph, {{{1, 10}}, {{1, 11}, {2, 20}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals<observation>({
        observation{1, 0, 1, 0, 10, 0, 10, 0, 11, 0, 11, 0},
        observation{2, 1, 3, 0, 20, 11, 31, 0, 22, 12, 34, 0},
        observation{0, 0, 0, 1, 0, 0, 0, 11, 0, 0, 0, 12},
    }));
}

TEST_CASE("a node can read one dictionary item",
          "[evaluation][tsd][feature:getitem][series:tsd][scalar:integer][binding:direct][transition:update]") {
    const auto select = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        auto changed = input.item_modified(1);
        input.for_each_added([&](const auto &key) { changed = changed || key.value() == 1; });
        if (input.contains(1) && changed) {
            output.set(*input[1]);
        }
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return select(input); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({10, 11, std::nullopt}));
}

namespace {

struct erase_output_keys_definition {
    Out<TSD<i64, TS<i64>>> output;

    void start() const {
        output[1].set(10);
        output[2].set(20);
        output[3].set(30);
        output[4].set(40);

        const auto first_id = output.item_id(1);
        const auto third_id = output.item_id(3);
        auto visited = std::vector<i64>{};
        output.for_each_key([&](const auto &key) {
            visited.push_back(key.value());
            if (key.value() % 2 == 0) {
                CHECK(output.erase(key));
            }
        });
        std::ranges::sort(visited);

        CHECK(visited == std::vector<i64>{1, 2, 3, 4});
        CHECK(output.item_id(1) == first_id);
        CHECK(output.item_id(3) == third_id);
        CHECK(output.item_id(2) == util::occupancy_index::npos);
        CHECK(output.item_id(4) == util::occupancy_index::npos);
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("a dictionary output can erase visited keys without changing retained item IDs",
          "[evaluation][tsd][feature:collection-change][series:tsd][scalar:integer]"
          "[transition:addition][transition:removal]") {
    const auto source = node<erase_output_keys_definition>();

    CHECK(bg::run_graph([&] { return source(); }).equals({{{1, 10}, {3, 30}}}));
}

TEST_CASE("a node can select a dictionary item using a time series key",
          "[evaluation][tsd][feature:getitem][series:tsd][series:value][scalar:integer]"
          "[binding:direct][transition:update]") {
    const auto select = node([](In<TSD<i64, TS<i64>>> input, i64 key) { return *input[key]; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input, wp<TS<i64>> key) {
        return select(input, key);
    };

    const auto actual = bg::run_graph(
        graph, {{{1, 10}, {2, 100}}, {{1, 11}, {2, 110}}, {{1, 12}, {2, 120}}}, {1, 2, 1});

    CHECK(actual.equals({10, 110, 12}));
}

TEST_CASE("a time series dictionary publishes item and key changes",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:addition][transition:removal]"
          "[transition:update][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    using namespace bg::literals;
    struct Snapshot {
        i64 size;
        i64 first_value;
        std::vector<i64> added;
        std::vector<i64> removed;
        std::vector<i64> modified;

        bool operator==(const Snapshot &) const = default;
    };

    auto snapshots = std::vector<Snapshot>{};

    const auto inspect = node([&](In<TSD<i64, TS<i64>>> input) {
        auto snapshot = Snapshot{.size = static_cast<i64>(input.size()),
                                 .first_value = input.contains(1) ? *input[1] : -1};
        input.for_each_added([&](const auto &key) { snapshot.added.push_back(key.value()); });
        input.for_each_removed([&](const auto &key) { snapshot.removed.push_back(key.value()); });
        input.for_each_modified([&](const auto &key) { snapshot.modified.push_back(key.value()); });
        std::ranges::sort(snapshot.added);
        std::ranges::sort(snapshot.removed);
        std::ranges::sort(snapshot.modified);
        const auto size = snapshot.size;
        snapshots.push_back(std::move(snapshot));
        return size;
    });

    const auto graph = bg::graph("name"_n = "dictionary_change_input",
                                 [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); });

    const auto actual = bg::run_graph(graph,
                                            {{{1, 10}, {2, 20}},
                                             {{1, 11}},
                                             {{1, 12}, {3, 30}},
                                             literal::SKIP,
                                             {{2, literal::REMOVE}, {3, literal::REMOVE}},
                                             {{2, 21}}});

    CHECK(actual.equals<i64>({2, 2, 3, std::nullopt, 1, 2}));
    CHECK(snapshots == std::vector<Snapshot>{
                           Snapshot{.size = 2, .first_value = 10, .added = {1, 2}, .modified = {1, 2}},
                           Snapshot{.size = 2, .first_value = 11, .modified = {1}},
                           Snapshot{.size = 3, .first_value = 12, .added = {3}, .modified = {1, 3}},
                           Snapshot{.size = 1, .first_value = 12, .removed = {2, 3}},
                           Snapshot{.size = 2, .first_value = 12, .added = {2}, .modified = {2}}});
}

TEST_CASE("a TSD input does not replay changes when another input triggers evaluation",
          "[evaluation][tsd][feature:collection-change][transition:addition][transition:removal]"
          "[input-guarantee:valid-items]") {
    struct observation {
        bool modified;
        i64 added;
        i64 removed;
        i64 updated;
        i64 changes;
        bool cursor_added;
        bool cursor_removed;
        bool cursor_updated;
        bool cursor_modified;
        bool found_added;
        bool found_removed;
        bool found_updated;
        bool found_modified;
        bool item_modified;
        i64 valid_items_added;
        i64 valid_items_removed;
        i64 removed_physical;

        bool operator==(const observation &) const = default;
    };

    const auto update = node([](In<TS<i64>> trigger, Out<TSD<i64, TS<i64>>> output) {
        if (*trigger == 1) {
            output[7].set(70);
        } else if (*trigger == 3) {
            output[7].set(71);
        } else if (*trigger == 5) {
            output.erase(7);
        }
    });
    const auto inspect = node([](In<TSD<i64, TS<i64>>> input, In<TSD<i64, TS<i64>>, valid_items> valid_input,
                                 In<TS<i64>>) {
        auto added = i64{};
        input.for_each_added([&](const auto &) { ++added; });
        auto removed = i64{};
        input.for_each_removed([&](const auto &) { ++removed; });
        auto updated = i64{};
        input.for_each_updated([&](const auto &) { ++updated; });
        auto changes = i64{};
        input.for_each_modified([&](const auto &) { ++changes; });
        auto valid_items_added = i64{};
        valid_input.for_each_added([&](const auto &) { ++valid_items_added; });
        auto valid_items_removed = i64{};
        valid_input.for_each_removed([&](const auto &) { ++valid_items_removed; });
        auto removed_physical = i64{};
        input.for_each_removed_physical_id([&](item_id_t) { ++removed_physical; });

        const auto raw_key = i64{7};
        const auto key = Scalar<i64>::from(raw_key);
        return observation{input.modified(),
                           added,
                           removed,
                           updated,
                           changes,
                           input.next_added_id(0) != util::occupancy_index::npos,
                           input.next_removed_id(0) != util::occupancy_index::npos,
                           input.next_updated_id(0) != util::occupancy_index::npos,
                           input.next_modified_id(0) != util::occupancy_index::npos,
                           input.find_added_id(key) != util::occupancy_index::npos,
                           input.find_removed_id(key) != util::occupancy_index::npos,
                           input.find_updated_id(key) != util::occupancy_index::npos,
                           input.find_modified_id(key) != util::occupancy_index::npos,
                           input.item_modified(key),
                           valid_items_added,
                           valid_items_removed,
                           removed_physical};
    });
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto dictionary = update(trigger);
        return inspect(dictionary, dictionary, trigger);
    };

    const auto actual = bg::run_graph(graph, {1, 2, 3, 4, 5, 6});

    CHECK(actual.equals<observation>({observation{.modified = true,
                                                  .added = 1,
                                                  .changes = 1,
                                                  .cursor_added = true,
                                                  .cursor_modified = true,
                                                  .found_added = true,
                                                  .found_modified = true,
                                                  .item_modified = true,
                                                  .valid_items_added = 1},
                                     observation{},
                                     observation{.modified = true,
                                                  .updated = 1,
                                                  .changes = 1,
                                                  .cursor_updated = true,
                                                  .cursor_modified = true,
                                                  .found_updated = true,
                                                  .found_modified = true,
                                                  .item_modified = true},
                                     observation{},
                                     observation{.modified = true,
                                                  .removed = 1,
                                                  .cursor_removed = true,
                                                  .found_removed = true,
                                                  .valid_items_removed = 1,
                                                  .removed_physical = 1},
                                     observation{}}));
}

namespace {

struct literal_dict_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSD<i64, TS<i64>>> output;

    void start(EvaluationContext *context) const {
        output.apply({{99, literal::SKIP}, {1, 10}, {2, 20}});
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.apply({{1, literal::REMOVE}});
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.apply({{1, 11}});
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("a TSD update removes an entry with REMOVE",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:removal]") {
    struct observation {
        bool first_present;
        bool second_present;
        bool skipped_key_present;
        i64 added;
        i64 modified;

        bool operator==(const observation &) const = default;
    };

    const auto source = node<literal_dict_source_definition>();
    auto removed_keys = std::vector<std::vector<i64>>{};

    const auto inspect = node([&](In<TSD<i64, TS<i64>>> input) {
        auto added = i64{};
        input.for_each_added([&](const auto &) { ++added; });

        auto removed = std::vector<i64>{};
        input.for_each_removed([&](const auto &key) { removed.push_back(key.value()); });

        removed_keys.push_back(removed);

        auto modified = i64{};
        input.for_each_modified([&](const auto &) { ++modified; });

        return observation{input.contains(1), input.contains(2), input.contains(99), added, modified};
    });
    const auto actual = bg::run_graph([&] { return inspect(source()); });

    CHECK(actual.equals<observation>({observation{true, true, false, 2, 2}, observation{false, true, false, 0, 0},
                                      observation{true, true, false, 1, 1}}));
    CHECK(removed_keys == std::vector<std::vector<i64>>{{}, {1}, {}});
}

TEST_CASE("a TSD update applies a root INVALID marker",
          "[evaluation][tsd][series:tsd][feature:collection-change][validity]") {
    const auto source = node([](In<TS<i64>, optional> input, Out<TSD<i64, TS<i64>>> output) {
        if (input.valid()) {
            output[1].set(input.value());
        } else {
            output.apply(literal::INVALID);
        }
    });

    CHECK(bg::run_graph(source, {11, literal::INVALID, 12})
              .equals({{{1, 11}}, literal::INVALID, {{1, 12}}}));
}

namespace {

struct dictionary_cursor_definition {
    In<TSD<i64, TS<i64>>> input;

    void evaluate() const {
        current.push_back(collect(&In<TSD<i64, TS<i64>>>::next_item_id));
        valid.push_back(collect(&In<TSD<i64, TS<i64>>>::next_valid_id));
        added.push_back(collect(&In<TSD<i64, TS<i64>>>::next_added_id));
        removed.push_back(collect(&In<TSD<i64, TS<i64>>>::next_removed_id));
        modified.push_back(collect(&In<TSD<i64, TS<i64>>>::next_modified_id));

        auto removed_physical_ids = std::vector<item_id_t>{};
        input.for_each_removed_physical_id([&](item_id_t id) { removed_physical_ids.push_back(id); });
        removed_physical.push_back(std::move(removed_physical_ids));
    }

    using Next = item_id_t (In<TSD<i64, TS<i64>>>::*)(item_id_t) const;

    std::vector<item_id_t> collect(Next next) const {
        auto result = std::vector<item_id_t>{};
        for (auto start = item_id_t{};;) {
            const auto id = (input.*next)(start);
            if (id == util::occupancy_index::npos) {
                return result;
            }
            result.push_back(id);
            start = id + 1;
        }
    }

    inline static std::vector<std::vector<item_id_t>> current;
    inline static std::vector<std::vector<item_id_t>> valid;
    inline static std::vector<std::vector<item_id_t>> added;
    inline static std::vector<std::vector<item_id_t>> removed;
    inline static std::vector<std::vector<item_id_t>> modified;
    inline static std::vector<std::vector<item_id_t>> removed_physical;
};

} // namespace

TEST_CASE("a time series dictionary cursor enumerates current and changed items",
          "[evaluation][tsd][feature:collection-change][series:tsd][scalar:integer]"
          "[transition:addition][transition:removal][binding:direct]") {
    using namespace bg::literals;

    const auto sink = node<dictionary_cursor_definition>();
    dictionary_cursor_definition::current.clear();
    dictionary_cursor_definition::valid.clear();
    dictionary_cursor_definition::added.clear();
    dictionary_cursor_definition::removed.clear();
    dictionary_cursor_definition::modified.clear();
    dictionary_cursor_definition::removed_physical.clear();

    const auto graph = bg::graph("name"_n = "dictionary_cursor_input",
                                 [&](wp<TSD<i64, TS<i64>>> input) { sink(input); });
    (void)bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(dictionary_cursor_definition::current ==
          std::vector<std::vector<item_id_t>>{{0, 1}, {0, 1}, {0}});
    CHECK(dictionary_cursor_definition::valid == std::vector<std::vector<item_id_t>>{{0, 1}, {0, 1}, {0}});
    CHECK(dictionary_cursor_definition::added == std::vector<std::vector<item_id_t>>{{0, 1}, {}, {}});
    CHECK(dictionary_cursor_definition::removed == std::vector<std::vector<item_id_t>>{{}, {}, {1}});
    CHECK(dictionary_cursor_definition::modified == std::vector<std::vector<item_id_t>>{{0, 1}, {0}, {}});
    CHECK(dictionary_cursor_definition::removed_physical == dictionary_cursor_definition::removed);
}

TEST_CASE("an invalid dictionary cursor has no item IDs",
          "[feature:collection-change][series:tsd]") {
    const auto input = In<TSD<i64, TS<i64>>>{};

    CHECK_FALSE(input.key_set().valid());
    CHECK(input.next_item_id(0) == util::occupancy_index::npos);
    CHECK(input.next_valid_id(0) == util::occupancy_index::npos);
    CHECK(input.next_added_id(0) == util::occupancy_index::npos);
    CHECK(input.next_removed_id(0) == util::occupancy_index::npos);
    CHECK(input.next_modified_id(0) == util::occupancy_index::npos);

    auto removed_physical_id_count = 0;
    input.for_each_removed_physical_id([&](item_id_t) { ++removed_physical_id_count; });
    CHECK(removed_physical_id_count == 0);
}

namespace {

struct valid_dictionary_cursor_definition {
    In<TSD<i64, TS<i64>>> input;

    void evaluate() const {
        auto keys = std::vector<i64>{};
        for (auto id = input.next_valid_id(0); id != util::occupancy_index::npos;
             id = input.next_valid_id(id + 1)) {
            keys.push_back(*input.key_at(id));
        }
        snapshots.push_back(std::move(keys));
    }

    inline static std::vector<std::vector<i64>> snapshots;
};

} // namespace

TEST_CASE("a time series dictionary valid cursor skips invalid items",
          "[evaluation][tsd][feature:validity][series:tsd][transition:invalidation]") {
    using namespace bg::literals;

    const auto sink = node<valid_dictionary_cursor_definition>();
    valid_dictionary_cursor_definition::snapshots.clear();

    const auto graph = bg::graph("name"_n = "valid_dictionary_cursor_input",
                                 [&](wp<TSD<i64, TS<i64>>> input) { sink(input); });
    (void)bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, literal::INVALID}}});

    CHECK(valid_dictionary_cursor_definition::snapshots == std::vector<std::vector<i64>>{{1, 2}, {2}});
}

TEST_CASE("dictionary item queries distinguish addition from modification",
          "[evaluation][tsd][feature:collection-change][series:tsd][scalar:integer]"
          "[transition:addition][transition:update][binding:direct]") {
    struct item_changes {
        bool updated;
        bool modified;

        bool operator==(const item_changes &) const = default;
    };

    const auto modified = node([](In<TSD<i64, TS<i64>>> input) {
        return item_changes{input.item_updated(1), input.item_modified(1)};
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return modified(input); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals<item_changes>(
        {item_changes{false, true}, item_changes{true, true}, item_changes{false, false}}));
}

TEST_CASE("a value dictionary provides a source-backed reference representation",
          "[evaluation][tsd][reference][feature:reference][feature:validity][series:tsd][scalar:integer]"
          "[binding:value-to-reference][transition:update]") {
    auto item_became_valid = std::vector<bool>{};
    auto item_is_valid = std::vector<bool>{};

    const auto summarize = node([&](In<TSD<i64, REF<TS<i64>>>> input) {
        item_became_valid.push_back(input[1].became_valid());
        item_is_valid.push_back(input[1].valid());
        const auto resolved = details::ResolvedTimeSeries::resolve(input[1].value());
        return static_cast<int>(input.size() * 100) + *static_cast<const int *>(resolved.storage);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return summarize(input); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({210, std::nullopt, 111}));
    // The item reference remains valid after an unrelated dictionary change.
    CHECK(item_is_valid == std::vector<bool>{true, true});
    CHECK(item_became_valid == std::vector<bool>{true, false});
}

namespace {

const auto rekey_reference_dictionary = node([](In<TSD<i64, REF<TS<i64>>>> input,
                                                 Out<TSD<i64, REF<TS<i64>>>> output) {
    input.for_each_added([&](const auto &key) { output[key.value() * 10].copy(input[key]); });
    input.for_each_removed([&](const auto &key) { output.erase(key.value() * 10); });
});

} // namespace

TEST_CASE("rekeying a reference dictionary preserves item updates and removals",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:reference-to-value][binding:value-to-reference][transition:update][transition:removal]") {
    const auto summarize = node([](In<TSD<i64, TS<i64>>> input) {
        return static_cast<int>(input.size() * 100) + (input.contains(10) ? *input[10] : -1);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return summarize(rekey_reference_dictionary(input)); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({210, 211, 111}));
}

TEST_CASE("source dictionary item routes batch fan-out to reference dictionaries",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:reference-to-value][binding:value-to-reference][shape:fan-out][transition:update]") {
    const auto source = node(
        [](i64 input, Out<TSD<i64, TS<i64>>> output) {
            for (auto key = i64{}; key != 31; ++key) {
                output[key].set(input + static_cast<int>(key));
            }
            if (input == 2) {
                output[31].set(33);
            } else if (input == 3) {
                output.erase(31);
            }
        });

    const auto rekey = node(
        [](In<TSD<i64, REF<TS<i64>>>> input, i64 offset,
           Out<TSD<i64, REF<TS<i64>>>> output) {
            input.for_each_added([&](const auto &key) { output[key.value() + offset].copy(input[key]); });
            input.for_each_removed([&](const auto &key) { output.erase(key.value() + offset); });
        });

    const auto sum = node([](In<TSD<i64, TS<i64>>> input) {
        auto result = 0;
        input.for_each_value([&](const auto &value) { result += *value; });
        return result;
    });

    const auto add = node([](i64 left, i64 right) { return left + right; });

    const auto graph = [&](wp<TS<i64>> input) {
        const auto values = source(input);
        return add(sum(rekey(values, i64{100})), sum(rekey(values, i64{200})));
    };

    const auto actual = bg::run_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({992, 1120, 1116}));
}

TEST_CASE("a reference dictionary dereferences unrelated node outputs",
          "[evaluation][tsd][reference][feature:reference][series:tsd][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto scale = node([](i64 input) { return input * 10; });

    const auto references = node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<TSD<i64, REF<TS<i64>>>> output) {
        output[1].copy(first);
        output[2].copy(second);
    });

    const auto sum = node([](In<TSD<i64, TS<i64>>> input) { return *input[1] + *input[2]; });

    const auto graph = [&](wp<TS<i64>> input) { return sum(references(increment(input), scale(input))); };

    const auto actual = bg::run_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({12, 23, 34}));
}

TEST_CASE("an explicit dictionary reference input receives a recursively adapted target",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:update][transition:removal]") {
    const auto pass = node([](In<REF<TSD<i64, REF<TS<i64>>>>> input,
                              Out<REF<TSD<i64, REF<TS<i64>>>>> output) { output.copy(input); });

    const auto summarize = node([](In<TSD<i64, TS<i64>>> input) {
        return static_cast<int>(input.size() * 100) + *input[1];
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return summarize(pass(input)); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({210, 211, 111}));
}

TEST_CASE("a node observes dictionary item addition, removal, and restoration",
          "[evaluation][tsd][feature:getitem][series:tsd][scalar:integer][binding:direct][transition:addition]"
          "[transition:removal][transition:resurrection]") {
    const auto select = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        if (input.contains(3)) {
            output.set(*input[3]);
        } else {
            output.invalidate();
        }
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return select(input); };

    const auto actual = bg::run_graph(graph, {literal::SKIP, {{3, 30}}, {{3, literal::REMOVE}}, {{3, 31}}});

    CHECK(actual.equals({std::nullopt, 30, std::nullopt, 31}));
}

TEST_CASE("a node can publish a contained dictionary reference",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto select = node([](In<TSD<i64, REF<TS<i64>>>> input,
                                    Out<REF<TS<i64>>> output) { output.copy(input[10]); });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return select(rekey_reference_dictionary(input)); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({10, 11, std::nullopt}));
}

TEST_CASE("a node can read a collection-valued dictionary item",
          "[evaluation][tsd][tsl][feature:getitem][series:tsd][series:fixed-tsl][scalar:integer]"
          "[binding:direct][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto select = node([](In<TSD<i64, TSL<TS<i64>, 2>>> input,
                                Out<TSL<TS<i64>, 2>> output) {
        const auto item = input[1];
        output[0].set(*item[0]);
        output[1].set(*item[1]);
    });

    const auto graph = [&](wp<TSD<i64, TSL<TS<i64>, 2>>> input) {
        return sum(select(input));
    };

    const auto actual = bg::run_graph(
        graph, {{{1, {{0, 10}, {1, 20}}}}, {{1, {{0, 11}}}}});

    CHECK(actual.equals({30, 31}));
}

namespace {

struct resurrecting_selected_item_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSD<i64, TS<i64>>> output;

    void start(EvaluationContext *context) const {
        output[3].set(30);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(3);
        output[3].set(31);
    }
};

} // namespace

TEST_CASE("a node observes same-cycle dictionary item resurrection",
          "[evaluation][tsd][feature:getitem][series:tsd][scalar:integer][binding:direct]"
          "[transition:resurrection]") {
    const auto source = node<resurrecting_selected_item_definition>();

    const auto select = node([](In<TSD<i64, TS<i64>>> input) { return *input[3]; });
    const auto graph = [&] { return select(source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({30, 31}));
}

TEST_CASE("time series dictionaries can contain time series dictionaries",
          "[evaluation][tsd][series:tsd][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto select = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input) { return *input[1][2]; });

    const auto graph = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> input) {
        return select(input);
    };

    const auto actual = bg::run_graph(graph, {{{1, {{2, 12}}}}});

    CHECK(actual.equals({12}));
}

namespace {

template <template <typename, auto...> typename Field> struct DictionaryBundle {
    Field<TS<i64>> value;
};

} // namespace

TEST_CASE("time series dictionaries can contain runtime-sized time series",
          "[evaluation][tsd][series:tsd][tsb][series:tsb][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct][schema:named]") {
    const auto extract = node([](In<TSD<i64, TSB<DictionaryBundle>>> input) { return *input[1]->value; });

    const auto graph = [&](wp<TSD<i64, TSB<DictionaryBundle>>> input) { return extract(input); };

    const auto actual = bg::run_graph(graph, {{{1, {{"value", 12}}}}});

    CHECK(actual.equals({12}));
}

namespace {

struct canceled_add_source_definition {
    Out<TSD<i64, TS<i64>>> output;

    void start() const {
        output[1].set(10);
        output.erase(1);
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("adding and removing a new dictionary item in one invocation produces a tick",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:removal]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct][transition:addition]") {
    const auto source = node<canceled_add_source_definition>();

    const auto marker = node([](In<TSD<i64, TS<i64>>>) { return true; });
    const auto graph = [&] { return marker(source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({true}));
}

namespace {

struct resurrecting_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSD<i64, TS<i64>>> output;

    void start(EvaluationContext *context) const {
        output[1].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(1);
        output[1].set(20);
    }
};

} // namespace

TEST_CASE("resurrecting a dictionary item reports a modification",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:resurrection]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    struct observation {
        i64 size;
        i64 value;
        i64 added;
        i64 updated;
        i64 removed;
        i64 modified;

        bool operator==(const observation &) const = default;
    };

    const auto source = node<resurrecting_source_definition>();

    const auto inspect = node([](In<TSD<i64, TS<i64>>> input) {
        auto added = i64{};
        input.for_each_added([&](const auto &) { ++added; });
        auto updated = i64{};
        input.for_each_updated([&](const auto &) { ++updated; });
        auto removed = i64{};
        input.for_each_removed([&](const auto &) { ++removed; });
        auto modified = i64{};
        input.for_each_modified([&](const auto &) { ++modified; });
        return observation{static_cast<i64>(input.size()), *input[1], added, updated, removed, modified};
    });
    const auto actual = bg::run_graph([&] { return inspect(source()); });

    CHECK(actual.equals<observation>({observation{1, 10, 1, 0, 0, 1}, observation{1, 20, 0, 1, 0, 1}}));
}

namespace {

struct nested_item_resurrecting_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSD<i64, TSD<i64, TS<i64>>>> output;

    void start(EvaluationContext *context) const {
        output[1][2].set(12);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(1);
        output[1];
    }
};

} // namespace

TEST_CASE("resurrecting a dictionary collection item reports a modification",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:resurrection]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = node<nested_item_resurrecting_source_definition>();

    struct change_counts {
        i64 updated;
        i64 modified;

        bool operator==(const change_counts &) const = default;
    };

    const auto count = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input) {
        auto updated_count = int{};
        input.for_each_updated([&](const auto &) { ++updated_count; });
        auto modified_count = int{};
        input.for_each_modified([&](const auto &) { ++modified_count; });
        return change_counts{updated_count, modified_count};
    });
    const auto graph = [&] { return count(source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<change_counts>({change_counts{0, 1}, change_counts{1, 1}}));
}

TEST_CASE("a generic dictionary view uses its resolved item type",
          "[evaluation][tsd][series:tsd][generic][declaration:generic][feature:collection-change]"
          "[scalar:integer][representation:native][binding:direct]") {
    const auto copy = node([](In<TSD<i64, TS<SCALAR>>> input, Out<TSD<i64, TS<SCALAR>>> output) {
        input.for_each([&](const auto &key, const auto &item) { output[key].set_value(item.value_view()); });
    });

    const auto select = node([](In<TSD<i64, TS<SCALAR>>> input, Out<TS<SCALAR>> output) {
        output.set_value(input[1].value_view());
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return select(copy(input)); };

    const auto actual = bg::run_graph(graph, {{{1, 10}}});

    CHECK(actual.equals<i64>({10}));
}

TEST_CASE("a dictionary becomes valid on its first empty tick and remains valid",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct][transition:first-tick]") {
    struct observation {
        bool valid;
        i64 size;
        i64 added;
        i64 removed;

        bool operator==(const observation &) const = default;
    };

    const auto inspect = node([](In<TSD<i64, TS<i64>>, optional> input) {
        auto added = i64{};
        input.for_each_added([&](const auto &) { ++added; });
        auto removed = i64{};
        input.for_each_removed([&](const auto &) { ++removed; });
        return observation{input.valid(), static_cast<i64>(input.size()), added, removed};
    });

    using namespace bg::literals;

    const auto graph = bg::graph("name"_n = "empty_dictionary_input",
                                 [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); });
    const auto actual =
        bg::run_graph(graph, {{}, {{1, 10}, {2, 20}}, {{1, literal::REMOVE}, {2, literal::REMOVE}}});

    CHECK(actual.equals<observation>({observation{true, 0, 0, 0}, observation{true, 2, 2, 0},
                                     observation{true, 0, 0, 2}}));
}

namespace {

struct invalidating_dict_input_definition {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct preserving_dict_from_value_definition {
    static constexpr auto on_invalidation = preserve_output;

    i64 input;
    Out<TSD<i64, TS<i64>>> output;

    void evaluate() const { output[1].set(input); }
};

struct dict_from_value_definition {
    i64 input;
    Out<TSD<i64, TS<i64>>> output;

    void evaluate() const { output[1].set(input); }
};

} // namespace

TEST_CASE("a TSD node preserves its output when it declares preserve_output",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][validity:required]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]"
          "[invalidation:preserve-output]") {
    const auto source = node<invalidating_dict_input_definition>();
    const auto dict_from_value = node<preserving_dict_from_value_definition>();

    const auto select = node([](In<TSD<i64, TS<i64>>> input) { return *input[1]; });

    const auto graph = [&] { return select(dict_from_value(source())); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({1}));
}

TEST_CASE("a TSD node invalidates its output when a required input becomes invalid",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][validity:required]"
          "[transition:invalidation]") {
    const auto source = node<invalidating_dict_input_definition>();
    const auto dict_from_value = node<dict_from_value_definition>();
    const auto select = node([](In<TSD<i64, TS<i64>>> input) { return *input[1]; });

    const auto actual = bg::run_graph([&] { return select(dict_from_value(source())); });

    CHECK(actual.equals({1, std::nullopt}));
    CHECK(actual.visit_tick(1, [](const TimeSeriesType &, bool valid, util::buffer_reader &) {
        CHECK_FALSE(valid);
    }));
}

namespace {

struct dictionary_enumeration_snapshot {
    std::vector<i64> keys;
    std::vector<item_id_t> ids;
    std::vector<int> values;
    std::vector<std::pair<i64, int>> key_values;
    std::vector<std::pair<item_id_t, int>> id_values;
};

struct dictionary_enumeration_sink_definition {
    In<TSD<i64, TS<i64>>> input;

    void evaluate() const {
        auto snapshot = dictionary_enumeration_snapshot{};
        input.for_each_key([&](const auto &key) { snapshot.keys.push_back(*key); });
        input.for_each_id([&](item_id_t id) { snapshot.ids.push_back(id); });
        input.for_each_value([&](const auto &value) { snapshot.values.push_back(*value); });
        input.for_each_key_value(
            [&](const auto &key, const auto &value) { snapshot.key_values.emplace_back(*key, *value); });
        input.for_each_id_value(
            [&](item_id_t id, const auto &value) { snapshot.id_values.emplace_back(id, *value); });
        snapshots.push_back(std::move(snapshot));
    }

    inline static std::vector<dictionary_enumeration_snapshot> snapshots;
};

} // namespace

TEST_CASE("dictionary enumeration exposes keys, ids, values, and their associations",
          "[evaluation][series:tsd][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][binding:direct]") {
    using namespace bg::literals;

    const auto sink = node<dictionary_enumeration_sink_definition>();
    dictionary_enumeration_sink_definition::snapshots.clear();

    const auto graph = bg::graph("name"_n = "dictionary_enumeration_input",
                                 [&](wp<TSD<i64, TS<i64>>> input) { sink(input); });
    (void)bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}}, {{2, literal::REMOVE}}});

    REQUIRE(dictionary_enumeration_sink_definition::snapshots.size() == 3);
    auto first = dictionary_enumeration_sink_definition::snapshots.front();
    std::ranges::sort(first.keys);
    std::ranges::sort(first.values);
    std::ranges::sort(first.key_values);
    std::ranges::sort(first.ids);

    auto id_value_ids = std::vector<item_id_t>{};
    auto id_values = std::vector<int>{};
    for (const auto &[id, value] : first.id_values) {
        id_value_ids.push_back(id);
        id_values.push_back(value);
    }
    std::ranges::sort(id_value_ids);
    std::ranges::sort(id_values);

    CHECK(first.keys == std::vector<i64>{1, 2});
    CHECK(first.values == std::vector{10, 20});
    CHECK(first.key_values == std::vector<std::pair<i64, int>>{{1, 10}, {2, 20}});
    REQUIRE(first.ids.size() == 2);
    CHECK(first.ids[0] != first.ids[1]);
    CHECK(id_value_ids == first.ids);
    CHECK(id_values == first.values);

    auto last = dictionary_enumeration_sink_definition::snapshots.back();
    std::ranges::sort(last.keys);
    std::ranges::sort(last.values);
    CHECK(last.keys == std::vector<i64>{1});
    CHECK(last.values == std::vector{11});
}

TEST_CASE("an invalid dictionary input has no entries to enumerate",
          "[value][series:tsd][feature:time-series-view][validity:optional]") {
    const auto input = In<TSD<i64, TS<i64>>>{};
    auto calls = std::array<std::size_t, 5>{};

    input.for_each_key([&](const auto &) { ++calls[0]; });
    input.for_each_id([&](item_id_t) { ++calls[1]; });
    input.for_each_value([&](const auto &) { ++calls[2]; });
    input.for_each_key_value([&](const auto &, const auto &) { ++calls[3]; });
    input.for_each_id_value([&](item_id_t, const auto &) { ++calls[4]; });

    CHECK(calls == std::array<std::size_t, 5>{});
}

TEST_CASE("a dictionary view accepts its exact non-canonical key type",
          "[evaluation][series:tsd][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][transition:addition][transition:removal][binding:direct]") {
    const auto read = node([](In<TSD<i64, TS<i64>>> input) {
        return input.contains(7) && !input.item_updated(7) ? *input[7] : -1;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return read(input); };

    const auto actual = bg::run_graph(graph, {{{7, 70}}, {{7, literal::REMOVE}}});

    CHECK(actual.equals({70, -1}));
}

TEST_CASE("a dictionary input accepts its exact non-canonical key type",
          "[evaluation][feature:getitem][series:tsd][scalar:integer][binding:direct]") {
    const auto select = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        if (input.contains(7)) {
            output.set(*input[7]);
        } else {
            output.invalidate();
        }
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return select(input); };

    const auto actual = bg::run_graph(graph, {{{7, 70}}, {{7, literal::REMOVE}}});

    CHECK(actual.equals({70, std::nullopt}));
}
