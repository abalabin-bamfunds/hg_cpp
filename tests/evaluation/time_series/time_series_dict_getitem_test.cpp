#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("dictionary getitem follows one scalar key without reacting to unrelated items",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]"
          "[transition:removal][transition:resurrection]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> dictionary) { return dictionary[i64{1}]; };

    const auto actual = testing::test_graph(
        graph, {{{2, 100}}, {{2, 101}}, {{1, 10}}, {{1, 11}}, {{1, literal::REMOVE}}, {{1, 12}}});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt, 10, 11, std::nullopt, 12}));
}

TEST_CASE("dictionary getitem accepts a missing key from an empty value dictionary",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:invalidation]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> dictionary) { return dictionary[i64{1}]; };

    const auto actual = testing::test_graph(graph, {{}, {}});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem accepts a missing key from an empty reference dictionary",
          "[evaluation][series:tsd][series:reference][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:invalidation]") {
    const auto make_dictionary =
        node([](In<TS<i64>>, Out<TSD<i64, REF<TS<i64>>>> output) { output.clear(); });

    const auto graph = [&](wp<TS<i64>> input) { return make_dictionary(input)[i64{1}]; };

    const auto actual = testing::test_graph(graph, {1, 2});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem follows a missing key when it is added",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:revalidation]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> dictionary) { return dictionary[i64{1}]; };

    const auto actual = testing::test_graph(graph, {{}, {{1, 10}}, {{1, 20}}});

    CHECK(actual.equals<i64>({std::nullopt, 10, 20}));
}

TEST_CASE("dictionary getitem remains invalid when a value item is added and removed together",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:removal]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*input);
        output.erase(1);
    });

    const auto graph = [&](wp<TS<i64>> input) { return make_dictionary(input)[i64{1}]; };

    const auto actual = testing::test_graph(graph, {1, 2});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem does not evaluate when its selected value item ticks",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-reference][transition:update]") {
    const auto monitor = node([](In<REF<TS<i64>>>) { return i64{1}; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> dictionary) {
        return monitor(dictionary[i64{1}]);
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}}, {{1, 20}}, {{1, 30}}});

    CHECK(actual.equals({1, std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem changes its subscription with a time series key",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> dictionary, wp<TS<i64>> key) {
        return dictionary[key];
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}, {2, 100}}, {{1, 11}, {2, 110}}, {{1, 12}, {2, 120}}},
                                            {1, 2, 1});

    CHECK(actual.equals<i64>({10, 110, 12}));
}

TEST_CASE("dictionary getitem fans out one selected value item without observing another item",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator][shape:fan-out]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto sum = node([](i64 left, i64 right) { return left + right; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> dictionary) {
        return sum(dictionary[i64{1}], dictionary[i64{1}]);
    };

    const auto actual = testing::test_graph(graph, {{{1, 1}, {2, 10}}, {{1, 2}, {2, 20}}, {{1, 3}, {2, 30}}});

    CHECK(actual.equals({2, 4, 6}));
}

TEST_CASE("dictionary getitem independently selects distinct value items",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator][shape:fan-out]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto sum = node([](i64 left, i64 right) { return left + right; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> dictionary) {
        return sum(dictionary[i64{1}], dictionary[i64{2}]);
    };

    const auto actual = testing::test_graph(graph, {{{1, 1}, {2, 10}}, {{1, 2}, {2, 20}}, {{1, 3}, {2, 30}}});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("dictionary getitem releases its subscription while a time series key is invalid",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> dictionary, wp<TS<i64>> key) {
        return dictionary[key];
    };

    const auto actual = testing::test_graph(
        graph, {{{1, 10}, {2, 100}}, {{1, 11}, {2, 110}}, {{1, 12}, {2, 120}}, {{1, 13}, {2, 130}}},
        {1, literal::INVALID, 2, 1});

    CHECK(actual.equals<i64>({10, std::nullopt, 120, 13}));
}

TEST_CASE("dictionary getitem selects an item from a dictionary of references",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto scale = node([](i64 input) { return input * 10; });

    const auto make_dictionary =
        node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second, Out<TSD<i64, REF<TS<i64>>>> output) {
            output[1].copy(first);
            output[2].copy(second);
        });

    const auto graph = [&](wp<TS<i64>> input) {
        return make_dictionary(increment(input), scale(input))[i64{2}];
    };

    const auto actual = testing::test_graph(graph, {1, 2, 3});

    CHECK(actual.equals<i64>({10, 20, 30}));
}

TEST_CASE("dictionary getitem follows a replaced reference without evaluating for target updates",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:update]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto scale = node([](i64 input) { return input * 10; });

    const auto make_dictionary =
        node([](In<TS<bool>> select_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                Out<TSD<i64, REF<TS<i64>>>> output) { output[1].copy(*select_second ? second : first); });

    const auto monitor = node([](In<REF<TS<i64>>>) { return i64{1}; });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> select_second) {
        return monitor(make_dictionary(select_second, increment(input), scale(input))[i64{1}]);
    };

    const auto actual = testing::test_graph(graph, {1, 2, 3, 4}, {false, false, true, true});

    CHECK(actual.equals({1, std::nullopt, 1, std::nullopt}));
}

TEST_CASE("dictionary getitem restores a removed reference item after its only selected block was released",
          "[evaluation][series:tsd][series:reference][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]"
          "[transition:removal][transition:resurrection]") {
    const auto make_target = node([](i64 input) { return input; });
    const auto make_dictionary =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> target, Out<TSD<i64, REF<TS<i64>>>> output) {
            if (*command == 0) {
                output.erase(1);
            } else {
                output[1].copy(target);
            }
        });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> command) {
        return make_dictionary(command, make_target(input))[i64{1}];
    };

    const auto actual = testing::test_graph(graph, {10, 20, 30}, {1, 0, 1});

    CHECK(actual.equals<i64>({10, std::nullopt, 30}));
}

TEST_CASE("dictionary getitem restores a removed reference item while another item keeps its block live",
          "[evaluation][series:tsd][series:reference][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]"
          "[transition:removal][transition:resurrection]") {
    const auto make_target = node([](i64 input) { return input; });
    const auto make_dictionary =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> target, Out<TSD<i64, REF<TS<i64>>>> output) {
            output[2].copy(target);
            if (*command == 0) {
                output.erase(1);
            } else {
                output[1].copy(target);
            }
        });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> command) {
        return make_dictionary(command, make_target(input))[i64{1}];
    };

    const auto actual = testing::test_graph(graph, {10, 20, 30}, {1, 0, 1});

    CHECK(actual.equals<i64>({10, std::nullopt, 30}));
}

TEST_CASE("dictionary reference getitem ignores another item",
          "[evaluation][series:tsd][series:reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][shape:fan-out][transition:update]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto scale = node([](i64 input) { return input * 10; });

    const auto make_dictionary = node([](In<TS<bool>> select_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                         Out<TSD<i64, REF<TS<i64>>>> output) {
        output[1].copy(first);
        output[2].copy(*select_second ? second : first);
    });

    const auto monitor = node([](In<REF<TS<i64>>>) { return i64{1}; });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> select_second) {
        return monitor(make_dictionary(select_second, increment(input), scale(input))[i64{1}]);
    };

    const auto actual = testing::test_graph(graph, {1, 2, 3}, {false, true, false});

    CHECK(actual.equals({1, std::nullopt, std::nullopt}));
}

TEST_CASE("a value dictionary provides stable final item references",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:value-to-reference][guarantee:final]") {
    const details::TimeSeriesDictionaryRuntimeType *observed_runtime_type{};
    const auto inspect = node([&](In<TSD<i64, REF<TS<i64>>>> input, Out<TS<bool>> output) {
        observed_runtime_type = &static_cast<const details::TimeSeriesDictionaryRuntimeType &>(input.runtime_type());
        const auto &dictionary_type = static_cast<const TimeSeriesDictionaryType &>(input.runtime_type().type());
        const auto &dictionary_item_type = static_cast<const TimeSeriesReferenceType &>(dictionary_type.value_type());
        const auto item = input[1];
        const auto &type = static_cast<const TimeSeriesReferenceType &>(item.runtime_type().type());
        output.set(dictionary_item_type.guarantee().value() == TimeSeriesGuarantee::final &&
                   type.guarantee().value() == TimeSeriesGuarantee::final);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return inspect(input); };

    const auto actual = testing::test_graph(graph, {{{1, 1}}});

    CHECK(actual.equals({true}));
    REQUIRE(observed_runtime_type != nullptr);
    const auto &dictionary_type = static_cast<const TimeSeriesDictionaryType &>(observed_runtime_type->type());
    const auto &dictionary_item_type = static_cast<const TimeSeriesReferenceType &>(dictionary_type.value_type());
    CHECK(dictionary_item_type.guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("a dereferenced dictionary follows a target that is initially invalid",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:revalidation]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TS<i64>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto make_dictionary =
        node([](In<REF<TS<i64>>> target, Out<TSD<i64, REF<TS<i64>>>> output) { output[1].copy(target); });
    const auto read = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        if (input[1].valid()) {
            output.set(*input[1]);
        } else {
            output.invalidate();
        }
    });

    const auto graph = [&](wp<TS<i64>> command) { return read(make_dictionary(make_target(command))); };

    const auto actual = testing::test_graph(graph, {0, 10});

    CHECK(actual.equals({std::nullopt, 10}));
}

namespace {

struct dereferenced_dictionary_change_queries {
    In<TSD<i64, TS<i64>>> input;

    void evaluate() const {
        const i64 key_value{1};
        const auto key = Scalar<i64>::from(key_value);
        const auto id = input.find_item_id(key);
        auto modified_ids = std::vector<item_id_t>{};
        auto updated_ids = std::vector<item_id_t>{};
        auto modified_item_ids = std::vector<item_id_t>{};
        auto cursor_ids = std::vector<item_id_t>{};

        input.for_each_modified([&](const auto &modified_key) {
            if (*modified_key == *key) {
                modified_ids.push_back(input.find_item_id(modified_key));
            }
        });
        input.runtime_type().for_each_updated_item(
            input.last_updated_state(), input.data(), &updated_ids, [](void *context, item_id_t item_id) {
                static_cast<std::vector<item_id_t> *>(context)->push_back(item_id);
            });
        input.runtime_type().for_each_modified_item(
            input.last_updated_state(), input.data(), input.last_updated(), &modified_item_ids,
            [](void *context, TimeSeriesRuntimeType::ModifiedItem item) {
                static_cast<std::vector<item_id_t> *>(context)->push_back(item.id);
            });
        for (auto cursor = item_id_t{};;) {
            cursor = input.next_modified_id(cursor);
            if (cursor == util::occupancy_index::npos) {
                break;
            }
            cursor_ids.push_back(cursor);
            ++cursor;
        }

        changes.push_back({id, input.find_modified_id(key), input.item_modified(key),
                           id != util::occupancy_index::npos &&
                               input.runtime_type().item_updated(input.last_updated_state(), input.data(), id),
                           std::move(modified_ids), std::move(updated_ids), std::move(modified_item_ids),
                           std::move(cursor_ids)});
    }

    struct Change {
        item_id_t id;
        item_id_t found_modified_id;
        bool item_modified;
        bool item_updated;
        std::vector<item_id_t> modified_ids;
        std::vector<item_id_t> updated_ids;
        std::vector<item_id_t> modified_item_ids;
        std::vector<item_id_t> cursor_ids;

        bool operator==(const Change &) const = default;
    };

    inline static std::vector<Change> changes;
};

struct rooted_reference_item_sink {
    In<TSD<i64, REF<TS<i64>>>> input;

    void evaluate() const {
        const auto item = input[1];
        states.push_back({item.valid(), item.became_valid(), item.became_invalid(), item.modified()});
    }

    inline static std::vector<std::array<bool, 4>> states;
};

} // namespace

TEST_CASE("a dereferenced dictionary reports later target changes after adding its key",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:collection-change][transition:addition][transition:invalidation]"
          "[transition:revalidation][transition:update]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TS<i64>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto make_dictionary = node([](In<REF<TS<i64>>> target,
                                         Out<TSD<i64, REF<TS<i64>>>> output) { output[1].copy(target); });
    const auto read_item = node([](In<TS<i64>> item, Out<TS<i64>> output) {
        if (item.valid()) {
            output.set(*item);
        } else {
            output.invalidate();
        }
    });
    const auto observe_changes = node<dereferenced_dictionary_change_queries>();
    dereferenced_dictionary_change_queries::changes.clear();

    const auto graph = [&](wp<TS<i64>> command) {
        const auto dictionary = make_dictionary(make_target(command));
        observe_changes(dictionary);
        return read_item(dictionary[1]);
    };

    const auto actual = testing::test_graph(graph, {10, 0, 20, 21});

    CHECK(actual.equals<i64>({10, std::nullopt, 20, 21}));
    REQUIRE(dereferenced_dictionary_change_queries::changes.size() == 4);
    for (auto index = std::size_t{1}; index < dereferenced_dictionary_change_queries::changes.size(); ++index) {
        const auto &change = dereferenced_dictionary_change_queries::changes[index];
        REQUIRE(change.id != util::occupancy_index::npos);
        CHECK(change.found_modified_id == change.id);
        CHECK(change.item_modified);
        CHECK(change.item_updated);
        CHECK(change.modified_ids == std::vector<item_id_t>{change.id});
        CHECK(change.updated_ids == std::vector<item_id_t>{change.id});
        CHECK(change.modified_item_ids == std::vector<item_id_t>{change.id});
        CHECK(change.cursor_ids == std::vector<item_id_t>{change.id});
    }
}

TEST_CASE("a dereferenced dictionary reports one update when a stored reference changes",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:collection-change]"
          "[transition:update]") {
    const auto make_first = node([](i64 input) { return input; });

    const auto make_second = node([](i64 input) { return input + 100; });

    const auto make_dictionary = node([](In<TS<bool>> select_second, In<REF<TS<i64>>> first,
                                         In<REF<TS<i64>>> second,
                                         Out<TSD<i64, REF<TS<i64>>>> output) {
        output[1].copy(*select_second ? second : first);
    });
    const auto read_item = node([](i64 item) { return item; });
    const auto observe_changes = node<dereferenced_dictionary_change_queries>();
    dereferenced_dictionary_change_queries::changes.clear();

    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> select_second) {
        const auto dictionary = make_dictionary(select_second, make_first(input), make_second(input));
        observe_changes(dictionary);
        return read_item(dictionary[1]);
    };

    const auto actual = testing::test_graph(graph, {1, 2}, {false, true});

    CHECK(actual.equals<i64>({1, 102}));
    REQUIRE(dereferenced_dictionary_change_queries::changes.size() == 2);
    const auto &change = dereferenced_dictionary_change_queries::changes.back();
    REQUIRE(change.id != util::occupancy_index::npos);
    CHECK(change.found_modified_id == change.id);
    CHECK(change.item_modified);
    CHECK(change.item_updated);
    CHECK(change.modified_ids == std::vector<item_id_t>{change.id});
    CHECK(change.updated_ids == std::vector<item_id_t>{change.id});
    CHECK(change.modified_item_ids == std::vector<item_id_t>{change.id});
    CHECK(change.cursor_ids == std::vector<item_id_t>{change.id});
}

TEST_CASE("a dereferenced dictionary reports a removed item once",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:collection-change]"
          "[transition:removal]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TS<i64>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto make_dictionary = node([](In<TS<i64>> command, In<REF<TS<i64>>> target,
                                         Out<TSD<i64, REF<TS<i64>>>> output) {
        if (*command == 0) {
            output.erase(1);
        } else {
            output[1].copy(target);
        }
    });
    const auto read_item = node([](In<TS<i64>> item, Out<TS<i64>> output) {
        if (item.valid()) {
            output.set(*item);
        } else {
            output.invalidate();
        }
    });
    const auto observe_changes = node<dereferenced_dictionary_change_queries>();
    dereferenced_dictionary_change_queries::changes.clear();

    const auto graph = [&](wp<TS<i64>> command) {
        const auto dictionary = make_dictionary(command, make_target(command));
        observe_changes(dictionary);
        return read_item(dictionary[1]);
    };

    const auto actual = testing::test_graph(graph, {10, 0});

    CHECK(actual.equals<i64>({10, std::nullopt}));
    REQUIRE(dereferenced_dictionary_change_queries::changes.size() == 2);
    const auto &added = dereferenced_dictionary_change_queries::changes.front();
    const auto &removed = dereferenced_dictionary_change_queries::changes.back();
    REQUIRE(added.id != util::occupancy_index::npos);
    CHECK(removed.id == util::occupancy_index::npos);
    CHECK(removed.found_modified_id == util::occupancy_index::npos);
    CHECK_FALSE(removed.item_modified);
    CHECK_FALSE(removed.item_updated);
    CHECK(removed.modified_ids.empty());
    CHECK(removed.cursor_ids.empty());
    CHECK(std::count(removed.updated_ids.begin(), removed.updated_ids.end(), added.id) == 1);
    CHECK(std::count(removed.modified_item_ids.begin(), removed.modified_item_ids.end(), added.id) == 1);
}

TEST_CASE("a sampled dereferenced dictionary preserves physical IDs and remappings",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:item-id-remapping][transition:update]") {
    const auto select_dictionary = node(
        [](In<TS<bool>> select_second, In<REF<TSD<i64, REF<TS<i64>>>>> first,
           In<REF<TSD<i64, REF<TS<i64>>>>> second,
           Out<REF<TSD<i64, REF<TS<i64>>>>> output) { output.copy(*select_second ? second : first); });
    const auto inspect = node([](In<TS<bool>> select_second, In<TSD<i64, TS<i64>>> selected,
                                 In<TSD<i64, REF<TS<i64>>>> first,
                                 In<TSD<i64, REF<TS<i64>>>> second, Out<TS<i64>> output) {
        auto actual_remappings = std::vector<std::pair<item_id_t, item_id_t>>{};
        selected.for_each_item_id_remapping([&](item_id_t old_id, item_id_t new_id) {
            actual_remappings.emplace_back(old_id, new_id);
        });
        auto expected_remappings = std::vector<std::pair<item_id_t, item_id_t>>{};
        if (*select_second) {
            for (auto new_id = selected.next_item_id(0); new_id != util::occupancy_index::npos;
                 new_id = selected.next_item_id(new_id + 1)) {
                const auto old_id = first.find_item_id(selected.key_at(new_id));
                if (old_id != util::occupancy_index::npos && old_id != new_id) {
                    expected_remappings.emplace_back(old_id, new_id);
                }
            }
        }

        auto actual_removed = std::vector<item_id_t>{};
        selected.for_each_removed_physical_id([&](item_id_t id) { actual_removed.push_back(id); });
        auto expected_removed = std::vector<item_id_t>{};
        if (*select_second) {
            for (auto old_id = first.next_item_id(0); old_id != util::occupancy_index::npos;
                 old_id = first.next_item_id(old_id + 1)) {
                if (second.find_item_id(first.key_at(old_id)) == util::occupancy_index::npos) {
                    expected_removed.push_back(old_id);
                }
            }
        }

        const auto remappings_match = actual_remappings == expected_remappings &&
                                      selected.has_item_id_remapping() == !expected_remappings.empty();
        output.set(remappings_match && actual_removed == expected_removed ? 1 : 0);
    });

    const auto graph = [&](wp<TS<bool>> select_second, wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        const auto selected = select_dictionary(select_second, first, second);
        return inspect(select_second, selected, first, second);
    };

    const auto actual = testing::test_graph(graph, {false, true}, {{{10, 1}, {20, 1}, {40, 1}},
                                                                    {{10, 1}, {20, 1}, {40, 1}}},
                                            {{{20, 1}, {10, 1}, {30, 1}}, {{20, 1}, {10, 1}, {30, 1}}});

    CHECK(actual.equals<i64>({1, 1}));
}

TEST_CASE("a dereferenced dictionary follows target invalidation and revalidation",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[feature:validity]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TS<i64>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto make_dictionary =
        node([](In<REF<TS<i64>>> target, Out<TSD<i64, REF<TS<i64>>>> output) { output[1].copy(target); });
    const auto read = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        if (input[1].valid()) {
            output.set(*input[1]);
        } else {
            output.invalidate();
        }
    });
    const auto observe_rooted_item = node<rooted_reference_item_sink>();
    rooted_reference_item_sink::states.clear();

    const auto graph = [&](wp<TS<i64>> command) {
        const auto dictionary = make_dictionary(make_target(command));
        observe_rooted_item(dictionary);
        return read(dictionary);
    };

    const auto actual = testing::test_graph(graph, {10, 0, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
    // Target changes do not change the stable routed reference.
    CHECK(rooted_reference_item_sink::states == std::vector<std::array<bool, 4>>{
                                                    {true, true, false, true},
                                                });
}

TEST_CASE("a dictionary reference representation crosses into a nested graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-value]"
          "[transition:update]") {
    const auto read = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) { output.set(*input[1]); });
    const auto inner = graph("name"_n = "consume_dictionary_references",
                             [&](wp<TSD<i64, REF<TS<i64>>>> input) -> wp<TS<i64>> { return read(input); });

    const auto outer = [&](wp<TSD<i64, TS<i64>>> input, wp<TS<bool>> run) {
        return nested(inner, run, input);
    };

    const auto actual = testing::test_graph(outer, {{{1, 10}}, {{1, 20}}, {{1, 30}}}, {true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("an inner graph selects an outer dictionary reference across stop and restart",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-reference]"
          "[binding:reference-to-value][transition:stop][transition:restart]") {
    const auto read = node([](i64 input) { return input; });
    const auto inner = graph("name"_n = "select_outer_dictionary_item",
                             [&](wp<TSD<i64, REF<TS<i64>>>> input) -> wp<TS<i64>> {
                                 return input[i64{1}].as<TS<i64>>();
                             });

    const auto outer = [&](wp<TSD<i64, TS<i64>>> input, wp<TS<bool>> run) {
        return read(nested(inner, run, input));
    };

    const auto actual = testing::test_graph(outer, {{{1, 10}}, {{1, 20}}, {{1, 30}}, {{1, 40}}},
                                            {true, false, true, true});

    CHECK(actual.equals<i64>({10, std::nullopt, 30, 40}));
}

TEST_CASE("an inner dictionary reference representation crosses into its outer graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) { output[1].set(*input); });
    const auto select =
        node([](In<TSD<i64, REF<TS<i64>>>> input, Out<REF<TS<i64>>> output) { output.copy(input[1]); });
    const auto read = node([](i64 input) { return input; });
    const auto inner = graph("name"_n = "produce_dictionary", [&](wp<TS<i64>> input) -> wp<TSD<i64, TS<i64>>> {
        return make_dictionary(input);
    });

    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) { return read(select(nested(inner, run, input))); };

    const auto actual = testing::test_graph(outer, {10, 20, 30}, {true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("a dictionary reference representation connects two nested graphs",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:update]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) { output[1].set(*input); });
    const auto select =
        node([](In<TSD<i64, REF<TS<i64>>>> input, Out<REF<TS<i64>>> output) { output.copy(input[1]); });
    const auto read = node([](i64 input) { return input; });
    const auto producer =
        graph("name"_n = "produce_dictionary",
              [&](wp<TS<i64>> input) -> wp<TSD<i64, TS<i64>>> { return make_dictionary(input); });
    const auto consumer =
        graph("name"_n = "consume_dictionary_references",
              [&](wp<TSD<i64, REF<TS<i64>>>> input) -> wp<TS<i64>> { return read(select(input)); });

    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) {
        const auto dictionary = nested(producer, run, input);
        return nested(consumer, run, dictionary);
    };

    const auto actual = testing::test_graph(outer, {10, 20, 30}, {true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("an inner graph selects a dictionary reference from another inner graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][boundary][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:stop][transition:restart]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) { output[1].set(*input); });
    const auto producer =
        graph("name"_n = "produce_selected_dictionary",
              [&](wp<TS<i64>> input) -> wp<TSD<i64, TS<i64>>> { return make_dictionary(input); });
    const auto read = node([](i64 input) { return input; });
    const auto consumer = graph("name"_n = "select_inner_dictionary_item",
                                [&](wp<TSD<i64, REF<TS<i64>>>> input) -> wp<TS<i64>> {
                                    return input[i64{1}].as<TS<i64>>();
                                });

    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run_producer, wp<TS<bool>> run_consumer) {
        return read(nested(consumer, run_consumer, nested(producer, run_producer, input)));
    };

    const auto actual = testing::test_graph(outer, {10, 20, 30, 40, 50},
                                            {true, true, false, true, true},
                                            {true, false, true, true, true});

    CHECK(actual.equals<i64>({10, std::nullopt, std::nullopt, 40, 50}));
}

TEST_CASE("dictionary getitem ignores changes to another contained dictionary",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:update]") {
    const auto read = node([](i64 input) { return input; });

    const auto graph = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> dictionary) {
        return read(dictionary[i64{1}][i64{2}]);
    };

    const auto actual = testing::test_graph(
        graph, {{{1, {{2, 10}}}}, {{2, {{2, 100}}}}, {{1, {{2, 11}}}}});

    CHECK(actual.equals({10, std::nullopt, 11}));
}

TEST_CASE("dictionary getitem follows removal and restoration of a contained dictionary",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto read = node([](i64 input) { return input; });

    const auto graph = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> dictionary) {
        return read(dictionary[i64{1}][i64{2}]);
    };

    const auto actual = testing::test_graph(graph, {{{1, {{2, 10}}}}, {{1, literal::REMOVE}}, {{1, {{2, 20}}}}});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("a contained dictionary reference representation follows removal and restoration",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto make_dictionary =
        node([](In<TS<i64>> command, Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto select =
        node([](In<TSD<i64, REF<TS<i64>>>> input, Out<REF<TS<i64>>> output) { output.copy(input[2]); });
    const auto read = node([](i64 input) { return input; });

    const auto graph = [&](wp<TS<i64>> command) { return read(select(make_dictionary(command)[i64{1}])); };

    const auto actual = testing::test_graph(graph, {10, -1, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("dictionary getitem preserves a collection item's runtime representation",
          "[evaluation][series:tsd][series:fixed-tsl][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });

    const auto graph = [&](wp<TSD<i64, TSL<TS<i64>, 2>>> dictionary) {
        return sum(dictionary[i64{1}]);
    };

    const auto actual = testing::test_graph(
        graph, {{{1, {{0, 1}, {1, 10}}}}, {{1, {{0, 2}, {1, 20}}}}, {{1, {{0, 3}, {1, 30}}}}});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("dereferencing a dictionary item samples a new dictionary target",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto select = node([](In<TS<i64>> selection, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        if (*selection == 0) {
            output[0].copy(first);
        } else if (*selection == 1) {
            output[0].copy(second);
        } else {
            output[0].invalidate();
        }
    });
    const auto inspect = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto item = input[0];
        auto added = 0;
        auto removed = 0;
        auto modified = 0;
        item.for_each_added([&](const auto &key) { added |= 1 << *key; });
        item.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
        item.for_each_modified([&](const auto &key) { modified |= 1 << *key; });
        output.set(added * 100 + removed * 10 + modified);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second,
                           wp<TS<i64>> selection) {
        return inspect(select(selection, first, second));
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}},
                                            {{{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}},
                                            {0, 1, 2});

    CHECK(actual.equals({600, 824, 120}));
}

TEST_CASE("dictionary getitem follows a sampled dictionary item",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:getitem][binding:reference-to-reference][binding:reference-to-value]"
          "[shape:nested][transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto read = node([](i64 input) { return input; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second,
                           wp<TS<bool>> use_second) {
        return read(select(use_second, first, second)[i64{0}][i64{2}]);
    };

    const auto actual = testing::test_graph(graph, {{{2, 20}}, {{2, 20}}, {{2, 20}}},
                                            {{{2, 200}}, {{2, 200}}, {{2, 200}}}, {false, true, true});

    CHECK(actual.equals({20, 200, 200}));
}

TEST_CASE("a sampled dictionary cursor enumerates every removed key",
          "[evaluation][series:tsd][series:value][reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:removal]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    auto actual_ids = std::vector<std::vector<item_id_t>>{};
    auto expected_ids = std::vector<std::vector<item_id_t>>{};
    const auto inspect = node([&](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                                  In<TSD<i64, TS<i64>>> previous, Out<TS<i64>> output) {
        const auto sampled = input[0];
        auto removed = i64{};
        for (auto id = sampled.next_removed_id(0); id != util::occupancy_index::npos;
             id = sampled.next_removed_id(id + 1)) {
            removed = removed * 10 + *sampled.key_at(id);
        }

        auto actual = std::vector<item_id_t>{};
        auto expected = std::vector<item_id_t>{};
        sampled.for_each_removed([&](const auto &key) { expected.push_back(previous.find_item_id(key)); });
        sampled.for_each_removed_physical_id([&](item_id_t id) { actual.push_back(id); });
        actual_ids.push_back(std::move(actual));
        expected_ids.push_back(std::move(expected));
        output.set(removed);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second,
                           wp<TS<bool>> use_second) {
        return inspect(select(use_second, first, second), first);
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}, {2, 20}, {3, 30}}, {{1, 10}, {2, 20}, {3, 30}}},
                                            {{{3, 300}}, {{3, 300}}}, {false, true});

    CHECK(actual.equals({0, 12}));
    CHECK(actual_ids == expected_ids);
    CHECK(actual_ids == std::vector<std::vector<item_id_t>>{{}, {0, 1}});
}

TEST_CASE("all consumers see the same dictionary target sample",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input, Out<TS<i64>> output) {
        auto result = 0;
        input[0].for_each_added([&](const auto &key) { result += static_cast<int>(*key); });
        input[0].for_each_removed([&](const auto &key) { result += static_cast<int>(*key) * 10; });
        output.set(result);
    });
    const auto sum = node([](i64 left, i64 right) { return left + right; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second,
                           wp<TS<bool>> use_second) {
        const auto dictionary = select(use_second, first, second);
        return sum(inspect(dictionary), inspect(dictionary));
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}}, {{1, 10}}}, {{{2, 20}}, {{2, 20}}}, {false, true});

    CHECK(actual.equals({2, 24}));
}

TEST_CASE("a passive consumer does not replay an old dictionary target sample",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][activity:passive][transition:update]") {
    const auto select = node([](In<TS<i64>> selection, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[0].copy(*selection == 0 ? first : second);
    });
    const auto inspect = node(
        [](In<TSD<i64, TSD<i64, TS<i64>>>, passive> input, In<TS<i64>> trigger, Out<TS<i64>> output) {
            auto result = 0;
            input[0].for_each_added([&](const auto &key) { result += static_cast<int>(*key); });
            input[0].for_each_removed([&](const auto &key) { result += static_cast<int>(*key) * 10; });
            output.set(result + *trigger * 0);
        });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second,
                           wp<TS<i64>> selection, wp<TS<i64>> trigger) {
        const auto dictionary = select(selection, first, second);
        return inspect(dictionary, trigger);
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}}, {{1, 10}}, literal::SKIP},
                                            {{{2, 20}}, {{2, 20}}, literal::SKIP},
                                            {0, 1, literal::SKIP}, {1, literal::SKIP, 2});

    CHECK(actual.equals({1, std::nullopt, 0}));
}

TEST_CASE("a contained dictionary target sample recurses into modified items",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TSD<i64, TS<i64>>>>> first,
                                In<REF<TSD<i64, TSD<i64, TS<i64>>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TSD<i64, TS<i64>>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect =
        node([](In<TSD<i64, TSD<i64, TSD<i64, TS<i64>>>>> input, Out<TS<i64>> output) {
            const auto nested = input[0][2];
            auto added = 0;
            auto removed = 0;
            auto modified = 0;
            nested.for_each_added([&](const auto &key) { added += static_cast<int>(*key); });
            nested.for_each_removed([&](const auto &key) { removed += static_cast<int>(*key); });
            nested.for_each_modified([&](const auto &key) { modified += static_cast<int>(*key); });
            output.set(added * 10'000 + removed * 100 + modified);
        });

    const auto graph = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> first,
                           wp<TSD<i64, TSD<i64, TS<i64>>>> second, wp<TS<bool>> use_second) {
        return inspect(select(use_second, first, second));
    };

    const auto actual = testing::test_graph(graph, {{{2, {{10, 10}, {20, 20}}}}, {{2, {{10, 10}, {20, 20}}}}},
                                            {{{2, {{20, 200}, {30, 300}}}}, {{2, {{20, 200}, {30, 300}}}}},
                                            {false, true});

    CHECK(actual.equals({300'000, 301'020}));
}

TEST_CASE("a dictionary reference sample reports key-set changes",
          "[series:tsd][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][transition:addition][transition:removal]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        const auto keys = input.key_set();
        auto added = 0;
        auto removed = 0;
        auto item_added = 0;
        auto item_removed = 0;
        keys.for_each_added([&](const auto &key) { added += static_cast<int>(*key); });
        keys.for_each_removed([&](const auto &key) { removed += static_cast<int>(*key); });
        input.for_each_added([&](const auto &key) { item_added += static_cast<int>(*key); });
        input.for_each_removed([&](const auto &key) { item_removed += static_cast<int>(*key); });
        output.set(item_added * 10'000 + item_removed * 1'000 + added * 100 + removed * 10);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second,
                           wp<TS<bool>> use_second) {
        return inspect(select(use_second, first, second));
    };

    const auto actual = testing::test_graph(graph, {{{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}},
                                            {{{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}}, {false, true});

    REQUIRE(actual.size() == 2);
    CHECK(actual[0]->view().value() == 30'300);
    CHECK(actual[1]->view().value() == 31'310);
}

TEST_CASE("a sampled TSD reports item ID exchanges across a reference rebind",
          "[series:tsd][feature:reference][feature:sampling][feature:item-id-remapping]"
          "[transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TS<bool>> use_second, In<TSD<i64, TS<i64>>> input,
                                 In<TSD<i64, TS<i64>>> first,
                                 In<TSD<i64, TS<i64>>> second, Out<TS<i64>> output) {
        auto actual = std::vector<std::pair<item_id_t, item_id_t>>{};
        input.for_each_item_id_remapping([&](item_id_t old_id, item_id_t new_id) {
            actual.emplace_back(old_id, new_id);
        });

        auto expected = std::vector<std::pair<item_id_t, item_id_t>>{};
        if (*use_second) {
            for (auto new_id = input.next_item_id(0); new_id != util::occupancy_index::npos;
                 new_id = input.next_item_id(new_id + 1)) {
                const auto key = input.key_at(new_id);
                if (key.value() != second.key_at(new_id).value()) {
                    output.set(false);
                    return;
                }
                const auto old_id = first.find_item_id(key);
                if (old_id != util::occupancy_index::npos && old_id != new_id) {
                    expected.emplace_back(old_id, new_id);
                }
            }
        }
        const auto valid = actual == expected && input.has_item_id_remapping() == !expected.empty() &&
                           (!*use_second || expected.size() == 2);
        output.set(valid ? (*use_second ? 2 : 1) : (*use_second ? 20 : 10));
    });

    const auto graph = [&](wp<TS<bool>> use_second, wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        return inspect(use_second, select(use_second, first, second), first, second);
    };

    const auto actual = testing::test_graph(graph, {false, true}, {{{10, 10}, {20, 20}}, {{10, 10}, {20, 20}}},
                                            {{{20, 20}, {10, 10}}, {{20, 20}, {10, 10}}});

    REQUIRE(actual.size() == 2);
    REQUIRE(actual[0].has_value());
    REQUIRE(actual[1].has_value());
    CHECK(actual[0]->view().value() == 1);
    CHECK(actual[1]->view().value() == 2);
}

TEST_CASE("a sampled TSD does not report remapping for additions and removals at unchanged IDs",
          "[series:tsd][feature:reference][feature:sampling][feature:item-id-remapping]"
          "[transition:addition][transition:removal]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TS<bool>> use_second, In<TSD<i64, TS<i64>>> input,
                                 In<TSD<i64, TS<i64>>> first,
                                 In<TSD<i64, TS<i64>>> second, Out<TS<i64>> output) {
        auto actual = std::vector<std::pair<item_id_t, item_id_t>>{};
        input.for_each_item_id_remapping([&](item_id_t old_id, item_id_t new_id) {
            actual.emplace_back(old_id, new_id);
        });

        auto expected = std::vector<std::pair<item_id_t, item_id_t>>{};
        if (*use_second) {
            for (auto new_id = input.next_item_id(0); new_id != util::occupancy_index::npos;
                 new_id = input.next_item_id(new_id + 1)) {
                const auto key = input.key_at(new_id);
                const auto old_id = first.find_item_id(key);
                if (old_id != util::occupancy_index::npos && old_id != new_id) {
                    expected.emplace_back(old_id, new_id);
                }
            }
        }
        const auto added_key_value = i64{30};
        const auto removed_key_value = i64{20};
        const auto has_addition_and_removal =
            !*use_second ||
            (input.find_added_id(Scalar<i64>::from(added_key_value)) != util::occupancy_index::npos &&
             input.find_removed_id(Scalar<i64>::from(removed_key_value)) != util::occupancy_index::npos);
        const auto valid = actual == expected && !input.has_item_id_remapping() && has_addition_and_removal;
        output.set(valid ? (*use_second ? 2 : 1) : (*use_second ? 20 : 10));
    });

    const auto graph = [&](wp<TS<bool>> use_second, wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        return inspect(use_second, select(use_second, first, second), first, second);
    };

    const auto actual = testing::test_graph(graph, {false, true}, {{{10, 10}, {20, 20}}, {{10, 10}, {20, 20}}},
                                            {{{10, 10}, {30, 30}}, {{10, 10}, {30, 30}}});

    REQUIRE(actual.size() == 2);
    REQUIRE(actual[0].has_value());
    REQUIRE(actual[1].has_value());
    CHECK(actual[0]->view().value() == 1);
    CHECK(actual[1]->view().value() == 2);
}

TEST_CASE("a TSD reports no item ID remapping for ordinary additions and removals",
          "[series:tsd][feature:item-id-remapping][transition:addition][transition:removal]") {
    const auto inspect = node([](In<TS<i64>> command, In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        auto remapping_count = std::size_t{};
        input.for_each_item_id_remapping([&](item_id_t, item_id_t) { ++remapping_count; });
        const auto remapping_found = input.has_item_id_remapping() || remapping_count != 0;
        output.set(remapping_found ? 10 + *command : *command);
    });

    const auto graph = [&](wp<TS<i64>> command, wp<TSD<i64, TS<i64>>> input) {
        return inspect(command, input);
    };

    const auto actual = testing::test_graph(graph, {0, 1}, {{{10, 10}}, {{10, literal::REMOVE}, {20, 20}}});

    CHECK(actual.equals<i64>({0, 1}));
}

TEST_CASE("a dictionary reference follows a target that becomes valid after selection",
          "[series:tsd][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    const auto select = node([](In<TS<bool>> selected, In<REF<TSD<i64, TS<i64>>>> target,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        if (*selected) {
            output.copy(target);
        } else {
            output.invalidate();
        }
    });
    const auto inspect = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        output.set(input.contains(1) ? *input[1] : -1);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> target, wp<TS<bool>> selected) {
        return inspect(select(selected, target));
    };

    const auto actual = testing::test_graph(graph, {literal::SKIP, literal::SKIP, {{1, 10}}}, {false, true, true});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt, 10}));
}

TEST_CASE("a contained dictionary reference sample recurses into nested TSS changes",
          "[series:tsd][series:tss][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:addition][transition:removal]") {
    const auto select = node([](In<TS<bool>> use_second,
                                In<REF<TSD<i64, TSS<i64>>>> first,
                                In<REF<TSD<i64, TSS<i64>>>> second,
                                Out<TSD<i64, REF<TSD<i64, TSS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<i64, TSD<i64, TSS<i64>>>> input,
                                 Out<TS<i64>> output) {
        const auto nested = input[0][2];
        auto added = 0;
        auto removed = 0;
        nested.for_each_added([&](const auto &item) { added += static_cast<int>(*item); });
        nested.for_each_removed([&](const auto &item) { removed += static_cast<int>(*item); });
        output.set(added * 100 + removed);
    });

    const auto graph = [&](wp<TSD<i64, TSS<i64>>> first,
                           wp<TSD<i64, TSS<i64>>> second, wp<TS<bool>> use_second) {
        return inspect(select(use_second, first, second));
    };

    const auto actual = testing::test_graph(graph, {{{2, {10, 20}}}, literal::SKIP},
                                            {{{2, {20, 30}}}, literal::SKIP}, {false, true});

    CHECK(actual.equals({3000, 3010}));
}

TEST_CASE("a dictionary reference sample reports nested TSS changes for added and removed items",
          "[series:tsd][series:tss][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:addition][transition:removal]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TSS<i64>>>> first,
                                In<REF<TSD<i64, TSS<i64>>>> second,
                                Out<REF<TSD<i64, TSS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<i64, TSS<i64>>> input, Out<TS<i64>> output) {
        auto added_keys = 0;
        auto removed_keys = 0;
        auto added_items = 0;
        auto removed_items = 0;
        input.for_each_added([&](const auto &key) {
            added_keys += static_cast<int>(*key);
            input[key].for_each_added([&](const auto &item) { added_items += static_cast<int>(*item); });
        });
        input.for_each_removed([&](const auto &key) {
            removed_keys += static_cast<int>(*key);
            input.item_at(input.find_removed_id(key)).for_each_removed(
                [&](const auto &item) { removed_items += static_cast<int>(*item); });
        });
        output.set(added_keys * 1'000'000 + removed_keys * 100'000 + added_items * 100 + removed_items);
    });

    const auto graph = [&](wp<TSD<i64, TSS<i64>>> first,
                           wp<TSD<i64, TSS<i64>>> second, wp<TS<bool>> use_second) {
        return inspect(select(use_second, first, second));
    };

    const auto actual = testing::test_graph(graph, {{{1, {10, 20}}}, literal::SKIP},
                                            {{{2, {30, 40}}}, literal::SKIP}, {false, true});

    CHECK(actual.equals({1'003'000, 2'107'030}));
}

TEST_CASE("sampled dictionary queries work before change enumeration",
          "[series:tsd][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][transition:addition][transition:removal][transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TS<i64>> query, In<TSD<i64, TS<i64>>> input,
                                 Out<TS<i64>> output) {
        const i64 key_one_value{1};
        const i64 key_two_value{2};
        const auto key_one = Scalar<i64>::from(key_one_value);
        const auto key_two = Scalar<i64>::from(key_two_value);
        auto result = 0;
        switch (*query) {
        case 0:
            result = input.find_added_id(key_one) != util::occupancy_index::npos;
            break;
        case 1:
            result = input.find_removed_id(key_one) != util::occupancy_index::npos;
            break;
        case 2:
            result = input.find_modified_id(key_two) != util::occupancy_index::npos;
            break;
        case 3:
            result = input.item_modified(key_two);
            break;
        case 4: {
            const auto id = input.find_item_id(key_two);
            result = id != util::occupancy_index::npos &&
                     input.runtime_type().item_updated(input.last_updated_state(), input.data(), id);
            break;
        }
        case 5:
            input.runtime_type().for_each_updated_item(
                input.last_updated_state(), input.data(), &result,
                [](void *value, item_id_t) { ++*static_cast<int *>(value); });
            break;
        case 6:
            input.runtime_type().for_each_modified_item(
                input.last_updated_state(), input.data(), input.last_updated(), &result,
                [](void *value, TimeSeriesRuntimeType::ModifiedItem) { ++*static_cast<int *>(value); });
            break;
        case 7:
            result = input.runtime_type().item_updated(input.last_updated_state(), input.data(), key_set_item_id);
            break;
        default:
            break;
        }
        output.set(result);
    });

    const auto graph = [&](wp<TS<i64>> query, wp<TS<bool>> use_second,
                           wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second) {
        return inspect(query, select(use_second, first, second));
    };

    const auto actual = testing::test_graph(
        graph, {0, 1, 2, 3, 4, 5, 6, 7}, {false, true, false, true, false, true, false, true},
        {{{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}, {{1, 10}, {2, 20}},
         {{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}, {{1, 10}, {2, 20}}},
        {{{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}, {{2, 200}, {3, 300}},
         {{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}, {{2, 200}, {3, 300}}});

    CHECK(actual.equals({1, 1, 1, 1, 1, 4, 4, 1}));
}

TEST_CASE("a dictionary item samples nested graph stop and restart",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:nested-graph][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto producer =
        graph("name"_n = "sampled_dictionary_target",
              [&](wp<TS<i64>> input) -> wp<TSD<i64, TS<i64>>> { return make_dictionary(input); });
    const auto wrap = node([](In<REF<TSD<i64, TS<i64>>>, optional> target,
                              Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        if (target.valid()) {
            output[0].copy(target);
        } else {
            output[0].invalidate();
        }
    });
    const auto inspect = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto item = input[0];
        auto added = 0;
        auto removed = 0;
        item.for_each_added([&](const auto &key) { added |= 1 << *key; });
        item.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
        output.set(added * 100 + removed * 10);
    });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> run) { return inspect(wrap(nested(producer, run, input))); };

    const auto actual = testing::test_graph(graph, {1, 2, 3}, {true, false, true});

    CHECK(actual.equals({600, 60, 600}));
}

TEST_CASE("dictionary getitem releases and restores its subscription across a nested graph lifecycle",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:stop][transition:restart]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) { output[1].set(*input); });
    const auto inner = graph("name"_n = "dictionary", [&](wp<TS<i64>> input) -> wp<TSD<i64, TS<i64>>> {
        return make_dictionary(input);
    });

    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) -> wp<TS<i64>> {
        return nested(inner, run, input)[i64{1}].as<TS<i64>>();
    };

    const auto actual =
        testing::test_graph(outer, {1, 2, 3, 4, 5}, {true, true, false, true, true});

    CHECK(actual.equals<i64>({1, 2, std::nullopt, 4, 5}));
}
