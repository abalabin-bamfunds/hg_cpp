#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

const auto increment = node([](In<TS<i64>> value, Out<TS<i64>> output) {
    output.set(*value + 1);
});

const auto add_values = node([](In<TS<i64>, optional> left,
                                In<TS<i64>, optional> right,
                                Out<TS<i64>> output) {
    output.set((left.valid() ? *left : 0) + (right.valid() ? *right : 0));
});

const auto values_at_key_one = node([](In<TS<i64>> value,
                                       Out<TSD<std::int64_t, TS<i64>>> output) {
    output[1].set(*value);
});

TEST_CASE("map applies a graph to each TSD item",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[binding:value-to-reference][definition:function]") {
    const auto mapped = graph("name"_n = "increment_each",
                              [](wp<TS<i64>> value) { return increment(value); });
    const auto tested = [&](wp<TS<i64>> value) {
        return map(mapped, values_at_key_one(value))[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 4, 9});

    CHECK(actual.equals<bg::i64>({2, 5, 10}));
}

TEST_CASE("map invokes a generic graph function",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[definition:function]") {
    const auto tested = [&](wp<TS<i64>> value) {
        return map([](auto item) { return increment(item); }, values_at_key_one(value))
            [std::int64_t{1}]
                .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{2, 5});

    CHECK(actual.equals<bg::i64>({3, 6}));
}

TEST_CASE("map invokes a graph through runtime arguments",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:runtime]") {
    const auto add_key = node([](In<TS<i64>> value,
                                 In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key,
                                 Out<TS<i64>> output) {
        output.set(*value + static_cast<int>(*key));
    });
    const auto mapped = graph(
        "name"_n = "runtime_add_key",
        [add_key](wp<TS<i64>> value,
                  wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> key) {
            return add_key(value, key);
        });
    const auto tested = [&](wp<TS<i64>> value) {
        const auto arguments = std::vector<MapArgument>{
            {WiringArgument{values_at_key_one(value)}},
            {WiringArgument{util::symbol{"__key_arg__"}, std::int64_t{1}}},
        };
        const WiringTemplate &runtime_graph = mapped;
        const auto runtime_arguments = std::span<const MapArgument>{arguments};
        return map(runtime_graph, runtime_arguments)
            [std::int64_t{1}]
                .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{3, 7});

    CHECK(actual.equals<bg::i64>({4, 8}));
}

TEST_CASE("map passes a named argument positionally to a graph function",
          "[feature:map][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[series:tsd][shape:nested][definition:function]") {
    const auto tested = [&](wp<TS<i64>> value) {
        return map([](auto item) { return increment(item); },
                   "item"_n = values_at_key_one(value))[std::int64_t{1}]
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{2, 5});

    CHECK(actual.equals<bg::i64>({3, 6}));
}

TEST_CASE("map retains a named argument for a graph template",
          "[feature:map][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[series:tsd][series:tsb][schema:generic][shape:nested][definition:function]") {
    const auto named_identity = graph(
        "name"_n = "named_identity",
        [](wp<TSB<SCHEMA>, var_kwargs> arguments) {
            return wp<TS<i64>>{arguments["item"].time_series()};
        });
    const auto tested = [&](wp<TS<i64>> value) {
        return map(named_identity,
                   "item"_n = values_at_key_one(value))[std::int64_t{1}]
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{2, 5});

    CHECK(actual.equals<bg::i64>({2, 5}));
}

TEST_CASE("map infers a leading key for a graph function with an output",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:function]") {
    const auto add_key = node([](In<TS<i64>> value,
                                 In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key,
                                 Out<TS<i64>> output) {
        output.set(*value + static_cast<int>(*key));
    });
    const auto tested = [&](wp<TS<i64>> value) {
        return map([&](auto key, auto item) { return add_key(item, key); },
                   values_at_key_one(value))[std::int64_t{1}]
            .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{3, 7});

    CHECK(actual.equals<bg::i64>({4, 8}));
}

TEST_CASE("map passes a const key to an indexed graph parameter",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:function]") {
    const auto add_key = node([](In<TS<i64>> value,
                                 In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key,
                                 Out<TS<i64>> output) {
        output.set(*value + static_cast<int>(*key));
    });
    const auto tested = [&](wp<TS<i64>> value) {
        return map([&](auto item, auto key) { return add_key(item, key); },
                   values_at_key_one(value), "__key_arg__"_n = 1)
            [std::int64_t{1}]
                .as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{3, 7});

    CHECK(actual.equals<bg::i64>({4, 8}));
}

TEST_CASE("map uses the union of mapped TSD key sets",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto left = node([](In<TS<i64>> value, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*value);
    });
    const auto right = node([](In<TS<i64>> value, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[2].set(*value * 10);
    });
    const auto sum = graph("name"_n = "sum_available",
                           [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) {
        return add_values(lhs, rhs);
    });
    const auto tested = [&](wp<TS<i64>> value) {
        const auto result = map(sum, left(value), right(value));
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 3});

    CHECK(actual.equals<bg::i64>({10, 30}));
}

struct KeySourceGraph {
    wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> key;

    auto wire() const { return key; }
};

TEST_CASE("map runs a source graph for keys selected by a TSS",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const][definition:aggregate]") {
    const auto keys = node([](In<TS<bool>> present, Out<TSS<std::int64_t>> output) {
        if (*present) {
            output.insert(7);
        } else {
            output.erase(7);
        }
    });
    const auto source = graph<KeySourceGraph>();
    const auto tested = [&](wp<TS<bool>> present) {
        return map(source, keys(present))[std::int64_t{7}].as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };

    const auto actual = testing::test_graph(tested, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({7, std::nullopt, 7}));
}

TEST_CASE("map infers a leading key for a source graph function",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const][definition:function]") {
    const auto keys = node([](In<TS<bool>> present, Out<TSS<std::int64_t>> output) {
        if (*present) {
            output.insert(3);
        } else {
            output.erase(3);
        }
    });
    const auto tested = [&](wp<TS<bool>> present) {
        return map([](auto key) { return key; }, keys(present))[std::int64_t{3}]
            .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };

    const auto actual = testing::test_graph(tested, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({3, std::nullopt, 3}));
}

TEST_CASE("a map key schedules its consumer when a branch starts",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const][transition:addition][transition:removal][definition:function]") {
    const auto keys = node([](In<TS<bool>> present, Out<TSS<std::int64_t>> output) {
        if (*present) {
            output.insert(5);
        } else {
            output.erase(5);
        }
    });
    const auto copy_key = node([](In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key,
                                  Out<TS<std::int64_t>> output) {
        output.set(*key);
    });
    const auto tested = [&](wp<TS<bool>> present) {
        return map([&](auto key) { return copy_key(key); }, keys(present))[std::int64_t{5}].as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({5, std::nullopt, 5}));
}

struct RecordingSink {
    In<TS<i64>> value;

    inline static std::vector<int> values;

    void evaluate() const { values.push_back(*value); }
};

TEST_CASE("map supports sink graph functions",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[definition:function]") {
    const auto sink = node<RecordingSink>();
    RecordingSink::values.clear();

    const auto tested = [&](wp<TS<i64>> value) -> wp<TS<i64>> {
        map([&](auto item) { sink(item); }, values_at_key_one(value));
        return value;
    };
    const auto actual = testing::test_graph(tested, std::array{1, 2, 3});

    CHECK(actual.equals<bg::i64>({1, 2, 3}));
    CHECK(RecordingSink::values == std::vector{1, 2, 3});
}

TEST_CASE("map uses an explicitly selected TSD key set",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto values = node([](In<TS<i64>> value,
                                Out<TSD<std::int64_t, TS<i64>>> output) {
        output[1].set(*value);
        output[2].set(*value * 10);
    });
    const auto key_set = node([](In<TS<bool>> present,
                                 Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*present) {
            output[1].set(0);
        } else {
            output.erase(1);
        }
    });
    const auto select_value = graph(
        "name"_n = "select_value",
        [](wp<TS<i64>> value, wp<TS<i64>>) { return value; });
    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> present) {
        const auto result = map(select_value, values(value), key_set(present),
                                "__key_set_arg__"_n = 1);
        return result[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array{3, 4, 5}, std::array{true, false, true});

    CHECK(actual.equals<bg::i64>({3, std::nullopt, 5}));
}

TEST_CASE("map uses an explicitly selected TSS key set for a source graph",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const]") {
    const auto keys = node([](In<TS<bool>>, std::int64_t key, Out<TSS<std::int64_t>> output) {
        output.insert(key);
    });
    const auto tested = [&](wp<TS<bool>> trigger) {
        const auto result = map(
            [](auto, auto key) { return key; }, keys(trigger, 2), keys(trigger, 1),
            "__key_arg__"_n = 1, "__key_set_arg__"_n = 1);
        return result[std::int64_t{1}]
            .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };

    const auto actual = testing::test_graph(tested, std::array{true});

    CHECK(actual.equals<std::int64_t>({1}));
}

TEST_CASE("a pass-through TSD remains one input to every mapped graph",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:update]") {
    const auto factors = node([](In<TS<i64>> factor,
                                 Out<TSD<std::int64_t, TS<i64>>> output) {
        output[9].set(*factor);
    });
    const auto add_factor = graph(
        "name"_n = "add_factor",
        [](wp<TS<i64>> value, wp<TSD<std::int64_t, TS<i64>>> factor_by_key) {
            return add_values(value, factor_by_key[std::int64_t{9}].as<TS<i64>>());
        });
    const auto tested = [&](wp<TS<i64>> value, wp<TS<i64>> factor) {
        const auto result = map(add_factor, values_at_key_one(value),
                                pass_through(factors(factor)));
        return result[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array{2, 3, 4}, std::array{10, 20, 30});

    CHECK(actual.equals<bg::i64>({12, 23, 34}));
}

struct NamedMapKey {
    wp<TS<i64>> value;
    wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> selected_key;

    auto wire() const { return selected_key; }
};

TEST_CASE("map selects a key parameter by name",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:aggregate]") {
    const auto tested = [&](wp<TS<i64>> value) {
        const auto result = map(graph<NamedMapKey>(), values_at_key_one(value),
                                "__key_arg__"_n = "selected_key");
        return result[std::int64_t{1}]
            .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };

    const auto actual = testing::test_graph(tested, std::array{3, 8});

    CHECK(actual.equals<std::int64_t>({1, std::nullopt}));
}

TEST_CASE("map reads a TSD through a nested graph reference",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][series:reference]"
          "[shape:doubly-nested][binding:reference-to-reference]") {
    const auto build_values = graph(
        "name"_n = "build_values",
        [](wp<TS<i64>> value) { return values_at_key_one(value); });
    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        const auto values = nested(build_values, run, value);
        const auto result = map(
            [](auto item) { return increment(item); }, values);
        return result[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array{2, 5}, std::array{true, true});

    CHECK(actual.equals<bg::i64>({3, 6}));
}

TEST_CASE("map reads a TSS through a nested graph reference",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:reference]"
          "[shape:doubly-nested][binding:reference-to-value][guarantee:const]") {
    const auto keys = node([](In<TS<bool>> condition,
                              Out<TSS<std::int64_t>> output) {
        if (*condition) {
            output.insert(1);
        } else {
            output.erase(1);
        }
    });
    const auto build_keys = graph(
        "name"_n = "build_keys",
        [keys](wp<TS<bool>> present) {
            return keys(present);
        });
    const auto tested = [&](wp<TS<bool>> present, wp<TS<bool>> run) {
        const auto keys = nested(build_keys, run, present);
        const auto result = map(
            [](auto key) { return key; }, keys, "__key_arg__"_n = 0);
        return result[std::int64_t{1}]
            .as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    };

    const auto actual = testing::test_graph(
        tested, std::array{true, false, true}, std::array{true, true, true});

    CHECK(actual.equals<std::int64_t>({1, std::nullopt, 1}));
}

} // namespace
