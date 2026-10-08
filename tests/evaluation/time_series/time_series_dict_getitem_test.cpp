#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

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
    const auto make_dictionary = node([](In<TS<i64>> command, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*command < 0) {
            output.erase(1);
        } else if (*command >= 100) {
            output[2].set(*command);
        } else {
            output[1].set(*command);
        }
    });
    const auto graph = [&](wp<TS<i64>> command) { return make_dictionary(command)[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{100, 101, 10, 11, -1, 12});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt, 10, 11, std::nullopt, 12}));
}

TEST_CASE("dictionary getitem accepts a missing key from an empty value dictionary",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:invalidation]") {
    const auto make_dictionary = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<i64>>> output) { output.clear(); });
    const auto graph = [&](wp<TS<i64>> input) { return make_dictionary(input)[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem accepts a missing key from an empty reference dictionary",
          "[evaluation][series:tsd][series:reference][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:invalidation]") {
    const auto make_dictionary =
        node([](In<TS<i64>>, Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output.clear(); });
    const auto graph = [&](wp<TS<i64>> input) { return make_dictionary(input)[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem follows a missing key when it is added",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:revalidation]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*input > 0) {
            output[1].set(*input);
        } else {
            output.clear();
        }
    });
    const auto graph = [&](wp<TS<i64>> input) { return make_dictionary(input)[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{-1, 10, 20});

    CHECK(actual.equals<bg::i64>({std::nullopt, 10, 20}));
}

TEST_CASE("dictionary getitem remains invalid when a value item is added and removed together",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:removal]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*input);
        output.erase(1);
    });
    const auto graph = [&](wp<TS<i64>> input) { return make_dictionary(input)[std::int64_t{1}]; };

    const auto actual = testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem does not evaluate when its selected value item ticks",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-reference][transition:update]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto monitor = node([](In<REF<TS<i64>>>, Out<TS<i64>> output) { output.set(1); });
    const auto graph = [&](wp<TS<i64>> input) { return monitor(make_dictionary(input)[std::int64_t{1}]); };

    const auto actual = testing::test_graph(graph, std::array{10, 20, 30});

    CHECK(actual.equals({1, std::nullopt, std::nullopt}));
}

TEST_CASE("dictionary getitem changes its subscription with a time series key",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<std::int64_t>> key) { return make_dictionary(input)[key]; };

    const auto actual = testing::test_graph(graph, std::array{10, 11, 12}, std::array<std::int64_t, 3>{1, 2, 1});

    CHECK(actual.equals<bg::i64>({10, 110, 12}));
}

TEST_CASE("dictionary getitem fans out one selected value item without observing another item",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator][shape:fan-out]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto sum = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) { output.set(*left + *right); });
    const auto graph = [&](wp<TS<i64>> input) {
        const auto dictionary = make_dictionary(input);
        return sum(dictionary[std::int64_t{1}], dictionary[std::int64_t{1}]);
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({2, 4, 6}));
}

TEST_CASE("dictionary getitem independently selects distinct value items",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator][shape:fan-out]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto sum = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) { output.set(*left + *right); });
    const auto graph = [&](wp<TS<i64>> input) {
        const auto dictionary = make_dictionary(input);
        return sum(dictionary[std::int64_t{1}], dictionary[std::int64_t{2}]);
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("dictionary getitem releases its subscription while a time series key is invalid",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto make_key = node([](In<TS<i64>> input, Out<TS<std::int64_t>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> key) { return make_dictionary(input)[make_key(key)]; };

    const auto actual = testing::test_graph(graph, std::array{10, 11, 12, 13}, std::array{1, 0, 2, 1});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 120, 13}));
}

TEST_CASE("dictionary getitem selects an item from a dictionary of references",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]") {
    const auto increment = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input + 1); });
    const auto scale = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input * 10); });
    const auto make_dictionary =
        node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            output[1].copy(first);
            output[2].copy(second);
        });
    const auto graph = [&](wp<TS<i64>> input) {
        return make_dictionary(increment(input), scale(input))[std::int64_t{2}];
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals<bg::i64>({10, 20, 30}));
}

TEST_CASE("dictionary getitem follows a replaced reference without evaluating for target updates",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:update]") {
    const auto increment = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input + 1); });
    const auto scale = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input * 10); });
    const auto make_dictionary =
        node([](In<TS<bool>> select_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output[1].copy(*select_second ? second : first); });
    const auto monitor = node([](In<REF<TS<i64>>>, Out<TS<i64>> output) { output.set(1); });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> select_second) {
        return monitor(make_dictionary(select_second, increment(input), scale(input))[std::int64_t{1}]);
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3, 4}, std::array{false, false, true, true});

    CHECK(actual.equals({1, std::nullopt, 1, std::nullopt}));
}

TEST_CASE("dictionary getitem restores a removed reference item after its only selected block was released",
          "[evaluation][series:tsd][series:reference][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]"
          "[transition:removal][transition:resurrection]") {
    const auto make_target = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto make_dictionary =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> target, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            if (*command == 0) {
                output.erase(1);
            } else {
                output[1].copy(target);
            }
        });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> command) {
        return make_dictionary(command, make_target(input))[std::int64_t{1}];
    };

    const auto actual = testing::test_graph(graph, std::array{10, 20, 30}, std::array{1, 0, 1});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30}));
}

TEST_CASE("dictionary getitem restores a removed reference item while another item keeps its block live",
          "[evaluation][series:tsd][series:reference][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]"
          "[transition:removal][transition:resurrection]") {
    const auto make_target = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto make_dictionary =
        node([](In<TS<i64>> command, In<REF<TS<i64>>> target, Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
            output[2].copy(target);
            if (*command == 0) {
                output.erase(1);
            } else {
                output[1].copy(target);
            }
        });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> command) {
        return make_dictionary(command, make_target(input))[std::int64_t{1}];
    };

    const auto actual = testing::test_graph(graph, std::array{10, 20, 30}, std::array{1, 0, 1});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30}));
}

TEST_CASE("dictionary reference getitem ignores another item",
          "[evaluation][series:tsd][series:reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][shape:fan-out][transition:update]") {
    const auto increment = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input + 1); });
    const auto scale = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input * 10); });
    const auto make_dictionary = node([](In<TS<bool>> select_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                         Out<TSD<std::int64_t, REF<TS<i64>>>> output) {
        output[1].copy(first);
        output[2].copy(*select_second ? second : first);
    });
    const auto monitor = node([](In<REF<TS<i64>>>, Out<TS<i64>> output) { output.set(1); });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> select_second) {
        return monitor(make_dictionary(select_second, increment(input), scale(input))[std::int64_t{1}]);
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, std::array{false, true, false});

    CHECK(actual.equals({1, std::nullopt, std::nullopt}));
}

TEST_CASE("a value dictionary provides final item references",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:value-to-reference][guarantee:final]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto inspect = node([](In<TSD<std::int64_t, REF<TS<i64>>>> input, Out<TS<bool>> output) {
        const auto item = input[1];
        const auto &type = static_cast<const TimeSeriesReferenceType &>(item.runtime_type().type());
        output.set(type.guarantee().value() == TimeSeriesGuarantee::final);
    });
    const auto graph = [&](wp<TS<i64>> input) { return inspect(make_dictionary(input)); };

    const auto actual = testing::test_graph(graph, std::array{1});

    CHECK(actual.equals({true}));
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
        node([](In<REF<TS<i64>>> target, Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output[1].copy(target); });
    const auto read = node([](In<TSD<std::int64_t, TS<i64>>> input, Out<TS<i64>> output) {
        if (input[1].valid()) {
            output.set(*input[1]);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&](wp<TS<i64>> command) { return read(make_dictionary(make_target(command))); };

    const auto actual = testing::test_graph(graph, std::array{0, 10});

    CHECK(actual.equals({std::nullopt, 10}));
}

namespace {

struct rooted_reference_item_sink {
    In<TSD<std::int64_t, REF<TS<i64>>>> input;

    void evaluate() const {
        const auto item = input[1];
        states.push_back({item.valid(), item.became_valid(), item.became_invalid(), item.modified()});
    }

    inline static std::vector<std::array<bool, 4>> states;
};

} // namespace

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
        node([](In<REF<TS<i64>>> target, Out<TSD<std::int64_t, REF<TS<i64>>>> output) { output[1].copy(target); });
    const auto read = node([](In<TSD<std::int64_t, TS<i64>>> input, Out<TS<i64>> output) {
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

    const auto actual = testing::test_graph(graph, std::array{10, 0, 20});

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
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto read = node([](In<TSD<std::int64_t, TS<i64>>> input, Out<TS<i64>> output) { output.set(*input[1]); });
    const auto inner = graph("name"_n = "consume_dictionary_references",
                             [&](wp<TSD<std::int64_t, REF<TS<i64>>>> input) -> wp<TS<i64>> { return read(input); });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) { return nested(inner, run, make_dictionary(input)); };

    const auto actual = testing::test_graph(outer, std::array{10, 20, 30}, std::array{true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("an inner graph selects an outer dictionary reference across stop and restart",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-reference]"
          "[binding:reference-to-value][transition:stop][transition:restart]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto inner = graph("name"_n = "select_outer_dictionary_item",
                             [&](wp<TSD<std::int64_t, REF<TS<i64>>>> input) -> wp<TS<i64>> {
                                 return input[std::int64_t{1}].as<TS<i64>>();
                             });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) {
        return read(nested(inner, run, make_dictionary(input)));
    };

    const auto actual = testing::test_graph(outer, std::array{10, 20, 30, 40}, std::array{true, false, true, true});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30, 40}));
}

TEST_CASE("an inner dictionary reference representation crosses into its outer graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto select =
        node([](In<TSD<std::int64_t, REF<TS<i64>>>> input, Out<REF<TS<i64>>> output) { output.copy(input[1]); });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto inner = graph("name"_n = "produce_dictionary", [&](wp<TS<i64>> input) -> wp<TSD<std::int64_t, TS<i64>>> {
        return make_dictionary(input);
    });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) { return read(select(nested(inner, run, input))); };

    const auto actual = testing::test_graph(outer, std::array{10, 20, 30}, std::array{true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("a dictionary reference representation connects two nested graphs",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:update]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto select =
        node([](In<TSD<std::int64_t, REF<TS<i64>>>> input, Out<REF<TS<i64>>> output) { output.copy(input[1]); });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto producer =
        graph("name"_n = "produce_dictionary",
              [&](wp<TS<i64>> input) -> wp<TSD<std::int64_t, TS<i64>>> { return make_dictionary(input); });
    const auto consumer =
        graph("name"_n = "consume_dictionary_references",
              [&](wp<TSD<std::int64_t, REF<TS<i64>>>> input) -> wp<TS<i64>> { return read(select(input)); });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) {
        const auto dictionary = nested(producer, run, input);
        return nested(consumer, run, dictionary);
    };

    const auto actual = testing::test_graph(outer, std::array{10, 20, 30}, std::array{true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("an inner graph selects a dictionary reference from another inner graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][boundary][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:stop][transition:restart]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto producer =
        graph("name"_n = "produce_selected_dictionary",
              [&](wp<TS<i64>> input) -> wp<TSD<std::int64_t, TS<i64>>> { return make_dictionary(input); });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto consumer = graph("name"_n = "select_inner_dictionary_item",
                                [&](wp<TSD<std::int64_t, REF<TS<i64>>>> input) -> wp<TS<i64>> {
                                    return input[std::int64_t{1}].as<TS<i64>>();
                                });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run_producer, wp<TS<bool>> run_consumer) {
        return read(nested(consumer, run_consumer, nested(producer, run_producer, input)));
    };

    const auto actual = testing::test_graph(outer, std::array{10, 20, 30, 40, 50},
                                            std::array{true, true, false, true, true},
                                            std::array{true, false, true, true, true});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, std::nullopt, 40, 50}));
}

TEST_CASE("dictionary getitem ignores changes to another contained dictionary",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:update]") {
    const auto make_dictionary =
        node([](In<TS<i64>> command, Out<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> output) {
            if (*command >= 100) {
                output[2][2].set(*command);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&](wp<TS<i64>> command) {
        return read(make_dictionary(command)[std::int64_t{1}][std::int64_t{2}]);
    };

    const auto actual = testing::test_graph(graph, std::array{10, 100, 11});

    CHECK(actual.equals({10, std::nullopt, 11}));
}

TEST_CASE("dictionary getitem follows removal and restoration of a contained dictionary",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto make_dictionary =
        node([](In<TS<i64>> command, Out<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&](wp<TS<i64>> command) {
        return read(make_dictionary(command)[std::int64_t{1}][std::int64_t{2}]);
    };

    const auto actual = testing::test_graph(graph, std::array{10, -1, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("a contained dictionary reference representation follows removal and restoration",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto make_dictionary =
        node([](In<TS<i64>> command, Out<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto select =
        node([](In<TSD<std::int64_t, REF<TS<i64>>>> input, Out<REF<TS<i64>>> output) { output.copy(input[2]); });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&](wp<TS<i64>> command) { return read(select(make_dictionary(command)[std::int64_t{1}])); };

    const auto actual = testing::test_graph(graph, std::array{10, -1, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("dictionary getitem preserves a collection item's runtime representation",
          "[evaluation][series:tsd][series:fixed-tsl][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TSL<TS<i64>, 2>>> output) {
        output[1][0].set(*input);
        output[1][1].set(*input * 10);
    });
    const auto sum = node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) { output.set(*input[0] + *input[1]); });
    const auto graph = [&](wp<TS<i64>> input) { return sum(make_dictionary(input)[std::int64_t{1}]); };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("dereferencing a dictionary item samples a new dictionary target",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto make_first = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(10);
        output[2].set(20);
    });
    const auto make_second = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[2].set(200);
        output[3].set(300);
    });
    const auto select = node([](In<TS<i64>> selection, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TS<i64>>>>> output) {
        if (*selection == 0) {
            output[0].copy(first);
        } else if (*selection == 1) {
            output[0].copy(second);
        } else {
            output[0].invalidate();
        }
    });
    const auto inspect = node([](In<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto item = input[0];
        auto added = 0;
        auto removed = 0;
        auto modified = 0;
        item.for_each_added([&](const auto &key) { added |= 1 << *key; });
        item.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
        item.for_each_modified([&](const auto &key) { modified |= 1 << *key; });
        output.set(added * 100 + removed * 10 + modified);
    });
    const auto graph = [&](wp<TS<i64>> selection) {
        const auto first = make_first(selection);
        const auto second = make_second(selection);
        return inspect(select(selection, first, second));
    };

    const auto actual = testing::test_graph(graph, std::array{0, 1, 2});

    CHECK(actual.equals({600, 824, 120}));
}

TEST_CASE("dictionary getitem follows a sampled dictionary item",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:getitem][binding:reference-to-reference][binding:reference-to-value]"
          "[shape:nested][transition:update]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[2].set(20); });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[2].set(200); });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&](wp<TS<bool>> use_second) {
        return read(
            select(use_second, make_first(use_second), make_second(use_second))[std::int64_t{0}][std::int64_t{2}]);
    };

    const auto actual = testing::test_graph(graph, std::array{false, true, true});

    CHECK(actual.equals({20, 200, 200}));
}

TEST_CASE("a sampled dictionary cursor enumerates every removed key",
          "[evaluation][series:tsd][series:value][reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:removal]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(10);
        output[2].set(20);
        output[3].set(30);
    });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[3].set(300); });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto sampled = input[0];
        auto removed = i64{};
        for (auto id = sampled.next_removed_id(0); id != util::occupancy_index::npos;
             id = sampled.next_removed_id(id + 1)) {
            removed = removed * 10 + *sampled.key_at(id);
        }
        output.set(removed);
    });
    const auto graph = [&](wp<TS<bool>> use_second) {
        return inspect(select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({0, 12}));
}

TEST_CASE("all consumers see the same dictionary target sample",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(10); });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[2].set(20); });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TS<i64>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> input, Out<TS<i64>> output) {
        auto result = 0;
        input[0].for_each_added([&](const auto &key) { result += static_cast<int>(*key); });
        input[0].for_each_removed([&](const auto &key) { result += static_cast<int>(*key) * 10; });
        output.set(result);
    });
    const auto sum = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) { output.set(*left + *right); });
    const auto graph = [&](wp<TS<bool>> use_second) {
        const auto dictionary = select(use_second, make_first(use_second), make_second(use_second));
        return sum(inspect(dictionary), inspect(dictionary));
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({2, 24}));
}

TEST_CASE("a passive consumer does not replay an old dictionary target sample",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][activity:passive][transition:update]") {
    const auto make_first = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(10); });
    const auto make_second = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<i64>>> output) { output[2].set(20); });
    const auto select = node([](In<TS<i64>> selection, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TS<i64>>>>> output) {
        output[0].copy(*selection == 0 ? first : second);
    });
    const auto inspect = node(
        [](In<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>, passive> input, In<TS<i64>> trigger, Out<TS<i64>> output) {
            auto result = 0;
            input[0].for_each_added([&](const auto &key) { result += static_cast<int>(*key); });
            input[0].for_each_removed([&](const auto &key) { result += static_cast<int>(*key) * 10; });
            output.set(result + *trigger * 0);
        });
    const auto graph = [&](wp<TS<i64>> selection, wp<TS<i64>> trigger) {
        const auto dictionary = select(selection, make_first(selection), make_second(selection));
        return inspect(dictionary, trigger);
    };

    const auto actual = testing::test_graph(graph, std::array<std::optional<int>, 3>{0, 1, std::nullopt},
                                            std::array<std::optional<int>, 3>{1, std::nullopt, 2});

    CHECK(actual.equals({1, std::nullopt, 0}));
}

TEST_CASE("a contained dictionary target sample recurses into modified items",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> output) {
        output[2][10].set(10);
        output[2][20].set(20);
    });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> output) {
        output[2][20].set(200);
        output[2][30].set(300);
    });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>>> first,
                                In<REF<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect =
        node([](In<TSD<std::int64_t, TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>>> input, Out<TS<i64>> output) {
            const auto nested = input[0][2];
            auto added = 0;
            auto removed = 0;
            auto modified = 0;
            nested.for_each_added([&](const auto &key) { added += static_cast<int>(*key); });
            nested.for_each_removed([&](const auto &key) { removed += static_cast<int>(*key); });
            nested.for_each_modified([&](const auto &key) { modified += static_cast<int>(*key); });
            output.set(added * 10'000 + removed * 100 + modified);
        });
    const auto graph = [&](wp<TS<bool>> use_second) {
        return inspect(select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({300'000, 301'020}));
}

TEST_CASE("a dictionary reference sample reports key-set changes",
          "[series:tsd][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][transition:addition][transition:removal]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(10);
        output[2].set(20);
    });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[2].set(200);
        output[3].set(300);
    });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<REF<TSD<std::int64_t, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<std::int64_t, TS<i64>>> input, Out<TS<i64>> output) {
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
    const auto graph = [&](wp<TS<bool>> use_second) {
        return inspect(select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    REQUIRE(actual.size() == 2);
    CHECK(actual[0]->value() == 30'300);
    CHECK(actual[1]->value() == 31'310);
}

TEST_CASE("a sampled TSD reports item ID exchanges across a reference rebind",
          "[series:tsd][feature:reference][feature:sampling][feature:item-id-remapping]"
          "[transition:update]") {
    const auto first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[10].set(10);
        output[20].set(20);
    });
    const auto second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[20].set(20);
        output[10].set(10);
    });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<REF<TSD<std::int64_t, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TS<bool>> use_second, In<TSD<std::int64_t, TS<i64>>> input,
                                 In<TSD<std::int64_t, TS<i64>>> first,
                                 In<TSD<std::int64_t, TS<i64>>> second, Out<TS<i64>> output) {
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
    const auto graph = [&](wp<TS<bool>> use_second) {
        const auto first_dictionary = first(use_second);
        const auto second_dictionary = second(use_second);
        return inspect(use_second, select(use_second, first_dictionary, second_dictionary), first_dictionary,
                       second_dictionary);
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    REQUIRE(actual.size() == 2);
    REQUIRE(actual[0].has_value());
    REQUIRE(actual[1].has_value());
    CHECK(actual[0]->value() == 1);
    CHECK(actual[1]->value() == 2);
}

TEST_CASE("a sampled TSD does not report remapping for additions and removals at unchanged IDs",
          "[series:tsd][feature:reference][feature:sampling][feature:item-id-remapping]"
          "[transition:addition][transition:removal]") {
    const auto first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[10].set(10);
        output[20].set(20);
    });
    const auto second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[10].set(10);
        output[30].set(30);
    });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<REF<TSD<std::int64_t, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TS<bool>> use_second, In<TSD<std::int64_t, TS<i64>>> input,
                                 In<TSD<std::int64_t, TS<i64>>> first,
                                 In<TSD<std::int64_t, TS<i64>>> second, Out<TS<i64>> output) {
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
        const auto added_key_value = std::int64_t{30};
        const auto removed_key_value = std::int64_t{20};
        const auto has_addition_and_removal =
            !*use_second ||
            (input.find_added_id(Scalar<std::int64_t>::from(added_key_value)) != util::occupancy_index::npos &&
             input.find_removed_id(Scalar<std::int64_t>::from(removed_key_value)) != util::occupancy_index::npos);
        const auto valid = actual == expected && !input.has_item_id_remapping() && has_addition_and_removal;
        output.set(valid ? (*use_second ? 2 : 1) : (*use_second ? 20 : 10));
    });
    const auto graph = [&](wp<TS<bool>> use_second) {
        const auto first_dictionary = first(use_second);
        const auto second_dictionary = second(use_second);
        return inspect(use_second, select(use_second, first_dictionary, second_dictionary), first_dictionary,
                       second_dictionary);
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    REQUIRE(actual.size() == 2);
    REQUIRE(actual[0].has_value());
    REQUIRE(actual[1].has_value());
    CHECK(actual[0]->value() == 1);
    CHECK(actual[1]->value() == 2);
}

TEST_CASE("a TSD reports no item ID remapping for ordinary additions and removals",
          "[series:tsd][feature:item-id-remapping][transition:addition][transition:removal]") {
    const auto source = node([](In<TS<i64>> command, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*command == 0) {
            output[10].set(10);
        } else {
            output.erase(10);
            output[20].set(20);
        }
    });
    const auto inspect = node([](In<TS<i64>> command, In<TSD<std::int64_t, TS<i64>>> input, Out<TS<i64>> output) {
        auto remapping_count = std::size_t{};
        input.for_each_item_id_remapping([&](item_id_t, item_id_t) { ++remapping_count; });
        const auto remapping_found = input.has_item_id_remapping() || remapping_count != 0;
        output.set(remapping_found ? 10 + *command : *command);
    });
    const auto graph = [&](wp<TS<i64>> command) { return inspect(command, source(command)); };

    const auto actual = testing::test_graph(graph, std::array{i64{0}, i64{1}});

    CHECK(actual.equals<i64>({0, 1}));
}

TEST_CASE("a dictionary reference follows a target that becomes valid after selection",
          "[series:tsd][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    const auto make_target = node([](In<TS<i64>> command, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*command != 0) {
            output[1].set(*command);
        }
    });
    const auto select = node([](In<TS<bool>> selected, In<REF<TSD<std::int64_t, TS<i64>>>> target,
                                Out<REF<TSD<std::int64_t, TS<i64>>>> output) {
        if (*selected) {
            output.copy(target);
        } else {
            output.invalidate();
        }
    });
    const auto inspect = node([](In<TSD<std::int64_t, TS<i64>>> input, Out<TS<i64>> output) {
        output.set(input.contains(1) ? *input[1] : -1);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<bool>> selected) {
        return inspect(select(selected, make_target(command)));
    };

    const auto actual = testing::test_graph(graph, std::array{0, 0, 10}, std::array{false, true, true});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt, 10}));
}

TEST_CASE("a contained dictionary reference sample recurses into nested TSS changes",
          "[series:tsd][series:tss][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:addition][transition:removal]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TSS<std::int64_t>>> output) {
        output[2].insert(10);
        output[2].insert(20);
    });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TSS<std::int64_t>>> output) {
        output[2].insert(20);
        output[2].insert(30);
    });
    const auto select = node([](In<TS<bool>> use_second,
                                In<REF<TSD<std::int64_t, TSS<std::int64_t>>>> first,
                                In<REF<TSD<std::int64_t, TSS<std::int64_t>>>> second,
                                Out<TSD<std::int64_t, REF<TSD<std::int64_t, TSS<std::int64_t>>>>> output) {
        output[0].copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<std::int64_t, TSD<std::int64_t, TSS<std::int64_t>>>> input,
                                 Out<TS<i64>> output) {
        const auto nested = input[0][2];
        auto added = 0;
        auto removed = 0;
        nested.for_each_added([&](const auto &item) { added += static_cast<int>(*item); });
        nested.for_each_removed([&](const auto &item) { removed += static_cast<int>(*item); });
        output.set(added * 100 + removed);
    });
    const auto graph = [&](wp<TS<bool>> use_second) {
        return inspect(select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({3000, 3010}));
}

TEST_CASE("a dictionary reference sample reports nested TSS changes for added and removed items",
          "[series:tsd][series:tss][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:addition][transition:removal]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TSS<std::int64_t>>> output) {
        output[1].insert(10);
        output[1].insert(20);
    });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TSS<std::int64_t>>> output) {
        output[2].insert(30);
        output[2].insert(40);
    });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TSS<std::int64_t>>>> first,
                                In<REF<TSD<std::int64_t, TSS<std::int64_t>>>> second,
                                Out<REF<TSD<std::int64_t, TSS<std::int64_t>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TSD<std::int64_t, TSS<std::int64_t>>> input, Out<TS<i64>> output) {
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
    const auto graph = [&](wp<TS<bool>> use_second) {
        return inspect(select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({1'003'000, 2'107'030}));
}

TEST_CASE("sampled dictionary queries work before change enumeration",
          "[series:tsd][series:value][feature:reference][feature:sampling]"
          "[binding:reference-to-value][transition:addition][transition:removal][transition:update]") {
    const auto make_first = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(10);
        output[2].set(20);
    });
    const auto make_second = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[2].set(200);
        output[3].set(300);
    });
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<std::int64_t, TS<i64>>>> first,
                                In<REF<TSD<std::int64_t, TS<i64>>>> second,
                                Out<REF<TSD<std::int64_t, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto inspect = node([](In<TS<i64>> query, In<TSD<std::int64_t, TS<i64>>> input,
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
    const auto graph = [&](wp<TS<i64>> query, wp<TS<bool>> use_second) {
        return inspect(query, select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = testing::test_graph(graph, std::array{0, 1, 2, 3, 4, 5, 6, 7},
                                            std::array{false, true, false, true, false, true, false, true});

    CHECK(actual.equals({1, 1, 1, 1, 1, 4, 4, 1}));
}

TEST_CASE("a dictionary item samples nested graph stop and restart",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:nested-graph][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    const auto make_dictionary = node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto producer =
        graph("name"_n = "sampled_dictionary_target",
              [&](wp<TS<i64>> input) -> wp<TSD<std::int64_t, TS<i64>>> { return make_dictionary(input); });
    const auto wrap = node([](In<REF<TSD<std::int64_t, TS<i64>>>, optional> target,
                              Out<TSD<std::int64_t, REF<TSD<std::int64_t, TS<i64>>>>> output) {
        if (target.valid()) {
            output[0].copy(target);
        } else {
            output[0].invalidate();
        }
    });
    const auto inspect = node([](In<TSD<std::int64_t, TSD<std::int64_t, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto item = input[0];
        auto added = 0;
        auto removed = 0;
        item.for_each_added([&](const auto &key) { added |= 1 << *key; });
        item.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
        output.set(added * 100 + removed * 10);
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<bool>> run) { return inspect(wrap(nested(producer, run, input))); };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3}, std::array{true, false, true});

    CHECK(actual.equals({600, 60, 600}));
}

TEST_CASE("dictionary getitem releases and restores its subscription across a nested graph lifecycle",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:stop][transition:restart]") {
    const auto make_dictionary =
        node([](In<TS<i64>> input, Out<TSD<std::int64_t, TS<i64>>> output) { output[1].set(*input); });
    const auto inner = graph("name"_n = "dictionary", [&](wp<TS<i64>> input) -> wp<TSD<std::int64_t, TS<i64>>> {
        return make_dictionary(input);
    });
    const auto outer = [&](wp<TS<i64>> input, wp<TS<bool>> run) -> wp<TS<i64>> {
        return nested(inner, run, input)[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual =
        testing::test_graph(outer, std::array{1, 2, 3, 4, 5}, std::array{true, true, false, true, true});

    CHECK(actual.equals<bg::i64>({1, 2, std::nullopt, 4, 5}));
}
