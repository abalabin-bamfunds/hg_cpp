#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct MapReferenceRow {
    Field<TS<i64>> value;
    Field<TS<string>> label;
};

const auto increment = node([](i64 value) { return value + 1; });

template <template <typename, auto...> typename Field> struct MapGetattrBundle {
    Field<i64> value;
};

TEST_CASE("map applies a graph to each TSD item",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[binding:value-to-reference][definition:function]") {
    const auto mapped = graph("name"_n = "increment_each",
                              [](wp<TS<i64>> value) { return increment(value); });

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return copy(map(mapped, values));
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}, {2, 10}}, {{1, 4}}, {{2, 20}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals({{{1, 2}, {2, 11}}, {{1, 5}}, {{2, 21}}, {{1, literal::REMOVE}}}));
}

TEST_CASE("map removes an output item while its result is invalid when valid_items is enabled",
          "[feature:map][feature:nested-graph][series:tsd][input-guarantee:valid-items]"
          "[transition:invalidation][transition:revalidation][transition:removal]") {
    const auto identity = graph("name"_n = "map_valid_items_identity", [](wp<TS<i64>> value) { return value; });
    const auto build_values = node([](i64 command, Out<TSD<i64, TS<i64>>> output) {
        if (command == 1) {
            output[1].set(10);
        } else if (command == 2) {
            output[1].invalidate();
            output[2].invalidate();
        } else if (command == 3) {
            output.erase(1);
            output[2].set(20);
        } else if (command == 4) {
            output[2].invalidate();
        } else {
            output[2].set(30);
        }
    });
    auto observed = std::vector<i64>{};
    const auto observe_keys = node([&observed](In<TS<i64>>, In<TSD<i64, REF<TS<i64>>>, optional> output) {
        const auto membership = static_cast<i64>(output.contains(1)) + 2 * static_cast<i64>(output.contains(2));
        observed.push_back(membership);
        return membership;
    });

    const auto tested = [&](wp<TS<i64>> command) {
        const auto values = build_values(command);
        return observe_keys(command, map(identity, values, "__valid_items__"_n = true));
    };

    bg::run_graph(tested, std::array{1, 2, 3, 4, 5});

    CHECK(observed == std::vector<i64>{1, 0, 2, 0, 2});
}

TEST_CASE("map omits an item when its graph output is nothing",
          "[feature:map][feature:operator][feature:nested-graph][series:tsd]"
          "[input-guarantee:valid-items][guarantee:final][validity:optional]") {
    const auto nothing = graph("name"_n = "map_nothing_output",
                               [](wp<TS<i64>>) -> wp<TS<i64, TimeSeriesGuarantee::final>> {
        return operators::nothing[type<TS<i64>>]().as<TS<i64, TimeSeriesGuarantee::final>>();
    });
    const auto build_values = node([](i64 value, Out<TSD<i64, TS<i64>>> output) { output[1].set(value); });
    auto observed = std::vector<bool>{};
    const auto observe = node([&observed](In<TS<bool>> trigger,
                                          In<TSD<i64, REF<TS<i64>>>, optional> output) {
        const auto contains = output.contains(1);
        observed.push_back(contains);
        return *trigger && !contains;
    });
    const auto tested = [&](wp<TS<bool>> trigger, wp<TS<i64>> value) {
        return observe(trigger, map(nothing, build_values(value), "__valid_items__"_n = true));
    };

    CHECK(bg::run_graph(tested, {true, false, true}, {10, 20, 30}).equals({true, false, true}));
    CHECK(observed == std::vector<bool>{false, false, false});
}

TEST_CASE("map valid_items mode does not share runtime identity with ordinary output mode",
          "[feature:map][feature:nested-graph][series:tsd][input-guarantee:valid-items]"
          "[transition:invalidation]") {
    const auto identity = graph("name"_n = "map_valid_items_identity_cache", [](wp<TS<i64>> value) { return value; });
    const auto build_values = node([](i64 command, Out<TSD<i64, TS<i64>>> output) {
        if (command == 1) {
            output[1].set(10);
        } else if (command == 2) {
            output[1].invalidate();
        } else {
            output[1].set(20);
        }
    });
    auto observed = std::vector<i64>{};
    const auto observe_keys = node([&observed](In<TS<i64>>, In<TSD<i64, REF<TS<i64>>>, optional> filtered,
                                               In<TSD<i64, REF<TS<i64>>>, optional> ordinary) {
        const auto membership = static_cast<i64>(filtered.contains(1)) + 2 * static_cast<i64>(ordinary.contains(1));
        observed.push_back(membership);
        return membership;
    });

    const auto tested = [&](wp<TS<i64>> command) {
        const auto values = build_values(command);
        const auto filtered = map(identity, values, "__valid_items__"_n = true);
        const auto ordinary = map(identity, values, "__valid_items__"_n = false);
        return observe_keys(command, filtered, ordinary);
    };

    bg::run_graph(tested, std::array{1, 2, 3});

    CHECK(observed == std::vector<i64>{3, 2, 3});
}

TEST_CASE("map valid_items follows a switched reference and ignores stale targets",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd]"
          "[input-guarantee:valid-items][transition:invalidation][transition:revalidation]"
          "[binding:reference-to-reference]") {
    const auto make_keys = node([](i64 value, Out<TSD<i64, TS<i64>>> output) { output[1].set(value); });
    const auto select = node([](In<TS<bool>> use_left, In<REF<TS<i64>>, optional> left,
                                In<REF<TS<i64>>, optional> right, Out<REF<TS<i64>>> output) {
        output.copy(*use_left ? left : right);
    });
    const auto apply = graph("name"_n = "map_valid_items_select_reference",
                             [&](wp<TS<i64>>, wp<TS<bool>> use_left, wp<TS<i64>, optional> left,
                                 wp<TS<i64>, optional> right) {
                                 return select(use_left, left, right);
                             });
    auto observed = std::vector<bool>{};
    auto right_validity = std::vector<bool>{};
    const auto observe_key = node([&observed, &right_validity](In<TS<i64>>, In<TS<i64>, optional> right,
                                                               In<TSD<i64, REF<TS<i64>>>, optional> output) {
        const auto contains = output.contains(1);
        observed.push_back(contains);
        right_validity.push_back(right.valid());
        return contains;
    });

    const auto tested = [&](wp<TS<i64>> key_value, wp<TS<bool>> use_left,
                            wp<TS<i64>, optional> left, wp<TS<i64>, optional> right) {
        const auto keys = make_keys(key_value);
        return observe_key(key_value, right,
                           map(apply, keys, pass_through(use_left), pass_through(left), pass_through(right),
                               "__valid_items__"_n = true));
    };

    bg::run_graph(
        tested, std::array{1, 2, 3, 4, 5, 6, 7, 8, 9},
        std::array{true, false, false, false, true, true, false, false, false},
        std::array<literal::TimeSeriesLiteral, 9>{literal::INVALID, literal::INVALID, 10, 10, 10, 10,
                                                  literal::INVALID, 11, 11},
        std::array<literal::TimeSeriesLiteral, 9>{literal::INVALID, literal::INVALID, literal::INVALID,
                                                  20, 20, 20, 20, literal::INVALID, 30});

    CHECK(observed == std::vector<bool>{false, false, false, true, true, true, true, false, true});
    CHECK(right_validity == std::vector<bool>{false, false, false, true, true, true, true, false, true});
}

TEST_CASE("map evaluates an operator template through ordinary arguments",
          "[feature:map][feature:nested-graph][feature:operator][feature:getattr]"
          "[series:tsd][shape:nested][definition:runtime]") {
    auto memory = allocator{};
    const auto first = bundle<MapGetattrBundle>(memory, i64{19});
    const auto second = bundle<MapGetattrBundle>(memory, i64{42});
    const auto values = node([&](Out<TSD<i64, TS<Bundle<MapGetattrBundle>>>> output) {
        output[1].set_value(first.view());
        output[2].set_value(second.view());
    });

    const auto tested = [&] {
        return map(operators::getattr, values(), util::symbol{"value"})[i64{2}];
    };

    const auto actual = bg::run_graph(tested);

    CHECK(actual.equals<i64>({42}));
}

TEST_CASE("map applies an inner graph's omitted scalar default",
          "[feature:map][feature:nested-graph][feature:wiring-defaults][series:tsd][shape:nested]"
          "[definition:function]") {
    const auto add = node([](i64 value, i64 amount) { return value + amount; });
    const auto add_default = graph(
        {"item"_n, "amount"_n = i64{2}},
        [=](wp<TS<i64>> item, i64 amount) { return add(item, amount); });
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return map(add_default, values)[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 5}}});

    CHECK(actual.equals<i64>({4, 7}));
}

TEST_CASE("map invokes a generic graph function",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[definition:function]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return map([](auto item) { return increment(item); }, values)
            [i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 5}}});

    CHECK(actual.equals<i64>({3, 6}));
}

TEST_CASE("a second map can consume a constant output reference from the first map",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:doubly-nested]"
          "[guarantee:const]") {
    const auto send = graph("name"_n = "send",
                            [](wp<TS<i64>>)
                                -> wp<TS<i64, TimeSeriesGuarantee::const_>> {
                                return operators::const_(i64{42}).as<TS<i64, TimeSeriesGuarantee::const_>>();
                            });
    const auto receive = graph(
        "name"_n = "receive",
        [](wp<TS<i64, TimeSeriesGuarantee::const_>> identity) { return identity; });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> requests) {
        return map(receive, map(send, requests))[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 7}}});

    CHECK(actual.equals<i64>({42}));
}

TEST_CASE("map valid_items retains a constant target",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd]"
          "[input-guarantee:valid-items][guarantee:const]") {
    const auto send = graph("name"_n = "send_const_valid_items",
                            [](wp<TS<i64>>) -> wp<TS<i64, TimeSeriesGuarantee::const_>> {
                                return operators::const_(i64{42}).as<TS<i64, TimeSeriesGuarantee::const_>>();
                            });
    const auto observe_key = node([](In<TSD<i64, REF<TS<i64, TimeSeriesGuarantee::const_>>>, optional> output) {
        return output.contains(1);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> requests) {
        return observe_key(map(send, requests, "__valid_items__"_n = true));
    };

    const auto actual = bg::run_graph(tested, {{{1, 7}}});

    CHECK(actual.equals<bool>({true}));
}

TEST_CASE("map republishes an explicit reference output when its target changes",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:nested]"
          "[binding:reference-to-reference][transition:update]") {
    const auto make_values = node([](i64 value, Out<TSD<i64, TS<i64>>> output) { output[1].set(value); });
    const auto scale = node([](i64 value, i64 factor) { return value * factor; });
    const auto choose = node([](bool use_left, In<REF<TS<i64>>> left, In<REF<TS<i64>>> right,
                                Out<REF<TS<i64>>> output) { output.copy(use_left ? left : right); });

    const auto apply = graph(
        "name"_n = "select_reference",
        [&](wp<TS<i64>>, wp<TS<bool>> use_left, wp<TS<i64>> left, wp<TS<i64>> right) {
            return choose(use_left, left, right);
        });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> use_left) {
        return map(apply, make_values(value), pass_through(use_left), pass_through(scale(value, 10)),
                   pass_through(scale(value, 100)))[i64{1}];
    };

    const auto actual = bg::run_graph(tested, std::array{1, 2, 3}, std::array{true, false, true});

    CHECK(actual.equals<i64>({10, 200, 30}));
}

TEST_CASE("map invokes a graph function with a const wiring-point reference parameter",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[definition:function]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto result = map([](const wp<TS<i64>> &item) { return increment(item); }, values);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 5}}});

    CHECK(actual.equals<i64>({3, 6}));
}

TEST_CASE("map generic graph functions preserve optional reference arguments",
          "[feature:map][feature:nested-graph][feature:reference][feature:wiring]"
          "[series:tsd][validity:optional][binding:reference-to-reference]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<i64>, optional> context) {
        const auto result = map(
            [](auto item, auto optional_context) {
                using OptionalContext = decltype(optional_context);
                static_assert(OptionalContext::validity == optional);
                CHECK(type_cast<TimeSeriesReferenceType>(&item.type()) != nullptr);
                CHECK(type_cast<TimeSeriesReferenceType>(&optional_context.type()) != nullptr);
                return increment(item);
            },
            values, pass_through(context));
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 2}}, {{1, 5}}, {{1, 9}}},
        {1, literal::SKIP, 3});

    CHECK(actual.equals<i64>({3, 6, 10}));
}

inline constexpr auto G = guarantee_variable<"G">;

TEST_CASE("map preserves a final item target through a nested dictionary reference",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:doubly-nested]"
          "[guarantee:final][binding:reference-to-reference]") {
    const auto make_dictionary = node([](i64 value,
                                         Out<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>> output) {
        output[1].set(value);
    });

    const auto read_value = node([](In<TS<i64, G>> value) { return *value; });

    const auto dictionary = graph("name"_n = "final_item_dictionary", [&](wp<TS<i64>> value) {
        return make_dictionary(value);
    });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        const auto nested_dictionary = nested(dictionary, run, value);
        const auto mapped = map(
            [&](auto item) {
                const auto &reference = type_cast<TimeSeriesReferenceType>(item.type());
                CHECK(&reference.target_type() == &TS<i64, TimeSeriesGuarantee::final>::type());
                CHECK(reference.guarantee().value() == TimeSeriesGuarantee::ordinary);
                return read_value(item);
            },
            nested_dictionary);
        return mapped[i64{1}];
    };

    CHECK(bg::run_graph(tested, std::array{17}, std::array{true}).equals<i64>({17}));
}

TEST_CASE("map invokes a graph through runtime arguments",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:runtime]") {
    const auto add_key = node([](i64 value, In<TS<i64, TimeSeriesGuarantee::const_>> key) {
        return value + static_cast<int>(*key);
    });

    const auto mapped = graph(
        "name"_n = "runtime_add_key",
        [add_key](wp<TS<i64>> value,
                  wp<TS<i64, TimeSeriesGuarantee::const_>> key) {
            return add_key(value, key);
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto arguments = std::vector<MapArgument>{
            {WiringArgument{values}},
            {WiringArgument{util::symbol{"__key_arg__"}, i64{1}}},
        };
        const WiringTemplate &runtime_graph = mapped;
        const auto runtime_arguments = std::span<const MapArgument>{arguments};
        return map(runtime_graph, runtime_arguments)
            [i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 3}}, {{1, 7}}});

    CHECK(actual.equals<i64>({4, 8}));
}

TEST_CASE("map passes a named argument positionally to a graph function",
          "[feature:map][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[series:tsd][shape:nested][definition:function]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return map([](auto item) { return increment(item); },
                   "item"_n = values)[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 5}}});

    CHECK(actual.equals<i64>({3, 6}));
}

TEST_CASE("map retains a named argument for a graph template",
          "[feature:map][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[series:tsd][series:tsb][schema:generic][shape:nested][definition:function]") {
    const auto named_identity = graph(
        "name"_n = "named_identity",
        [](wp<TSB<SCHEMA>, var_kwargs> arguments) {
            return wp<TS<i64>>{arguments["item"].time_series()};
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return map(named_identity,
                   "item"_n = values)[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 5}}});

    CHECK(actual.equals<i64>({2, 5}));
}

const auto values_at_key_one = node([](i64 value, Out<TSD<i64, TS<i64>>> output) {
    output[1].set(value);
});

TEST_CASE("map retains a named reference collection as a variadic graph argument",
          "[feature:map][feature:nested-graph][feature:variadic-wiring][feature:reference]"
          "[series:tsd][series:tsb][schema:generic][shape:doubly-nested][binding:reference-to-reference]") {
    const auto named_identity = graph(
        "name"_n = "named_identity_with_reference_collection",
        [](wp<TSB<SCHEMA>, var_kwargs> arguments) {
            const auto &context_type = arguments["context"].type();
            const auto *context_reference = type_cast<TimeSeriesReferenceType>(&context_type);
            CHECK(context_reference != nullptr);
            if (context_reference != nullptr) {
                CHECK(type_cast<TimeSeriesDictionaryType>(&context_reference->target_type()) != nullptr);
            }
            return wp<TS<i64>>{arguments["item"].time_series()};
        });

    const auto build_context = graph("name"_n = "build_context", [&](wp<TS<i64>> value) {
        return values_at_key_one(value);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                            wp<TS<i64>> context, wp<TS<bool>> run) {
        const auto reference_context = nested(build_context, run, context);
        const auto result = map(named_identity, "item"_n = values,
                                "context"_n = pass_through(reference_context));
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 5}}}, {10, 20}, {true, true});

    CHECK(actual.equals<i64>({2, 5}));
}

TEST_CASE("map infers a leading key for a graph function with an output",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:function]") {
    const auto add_key = node([](i64 value, In<TS<i64, TimeSeriesGuarantee::const_>> key) {
        return value + static_cast<int>(*key);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return map([&](auto key, auto item) { return add_key(item, key); },
                   values)[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 3}}, {{1, 7}}});

    CHECK(actual.equals<i64>({4, 8}));
}

TEST_CASE("map passes a const key to an indexed graph parameter",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:function]") {
    const auto add_key = node([](i64 value, In<TS<i64, TimeSeriesGuarantee::const_>> key) {
        return value + static_cast<int>(*key);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return map([&](auto item, auto key) { return add_key(item, key); },
                   values, "__key_arg__"_n = 1)
            [i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 3}}, {{1, 7}}});

    CHECK(actual.equals<i64>({4, 8}));
}

const auto add_values = node([](std::optional<i64> left, std::optional<i64> right) {
    return left.value_or(0) + right.value_or(0);
});

TEST_CASE("map uses the union of mapped TSD key sets",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto sum = graph("name"_n = "sum_available",
                           [](wp<TS<i64>> lhs, wp<TS<i64>> rhs) {
        return add_values(lhs, rhs);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> left, wp<TSD<i64, TS<i64>>> right) {
        const auto result = map(sum, left, right);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}}, {{1, 3}}}, {{{2, 10}}, {{2, 30}}});

    CHECK(actual.equals<i64>({10, 30}));
}

TEST_CASE("map runs a source graph for keys selected by a TSS",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const][definition:aggregate]") {
    struct KeySourceGraph {
        wp<TS<i64, TimeSeriesGuarantee::const_>> key;

        auto wire() const { return key; }
    };

    const auto source = graph<KeySourceGraph>();

    const auto tested = [&](wp<TSS<i64>> keys) {
        return map(source, keys)[i64{7}];
    };

    const auto actual = bg::run_graph(tested, {{7}, {literal::REMOVED(7)}, {7}});

    CHECK(actual.equals<i64>({7, std::nullopt, 7}));
}

TEST_CASE("map infers a leading key for a source graph function",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const][definition:function]") {
    const auto tested = [&](wp<TSS<i64>> keys) {
        return map([](auto key) { return key; }, keys)[i64{3}];
    };

    const auto actual = bg::run_graph(tested, {{3}, {literal::REMOVED(3)}, {3}});

    CHECK(actual.equals<i64>({3, std::nullopt, 3}));
}

TEST_CASE("a map key schedules its consumer when a branch starts",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const][transition:addition][transition:removal][definition:function]") {
    const auto copy_key = node([](In<TS<i64, TimeSeriesGuarantee::const_>> key) { return *key; });

    const auto tested = [&](wp<TSS<i64>> keys) {
        return map([&](auto key) { return copy_key(key); }, keys)[i64{5}];
    };

    const auto actual = bg::run_graph(tested, {{5}, {literal::REMOVED(5)}, {5}});

    CHECK(actual.equals<i64>({5, std::nullopt, 5}));
}

TEST_CASE("map supports sink graph functions",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[definition:function]") {
    auto recorded_values = std::vector<i64>{};

    const auto sink = node([&](i64 value) { recorded_values.push_back(value); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> input) {
        map([&](auto item) { sink(item); }, input);
        return input[i64{1}];
    };
    const auto actual = bg::run_graph(tested, {{{1, 1}}, {{1, 2}}, {{1, 3}}});

    CHECK(actual.equals<i64>({1, 2, 3}));
    CHECK(recorded_values == std::vector<i64>{1, 2, 3});
}

TEST_CASE("map uses an explicitly selected TSD key set",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto select_value = graph(
        "name"_n = "select_value",
        [](wp<TS<i64>> value, wp<TS<i64>>) { return value; });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                            wp<TSD<i64, TS<i64>>> key_set) {
        const auto result = map(select_value, values, key_set,
                                "__key_set_arg__"_n = 1);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 3}, {2, 30}}, {{1, 4}, {2, 40}}, {{1, 5}, {2, 50}}},
        {{{1, 0}}, {{1, literal::REMOVE}}, {{1, 0}}});

    CHECK(actual.equals<i64>({3, std::nullopt, 5}));
}

TEST_CASE("a nested map selects keys from a referenced mapped dictionary",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:doubly-nested]"
          "[transition:addition][transition:removal]") {
    const auto add = node([](i64 left, i64 right) { return left + right; });
    const auto combine = graph("name"_n = "combine_reference_dictionary_values",
                               [add](wp<TS<i64>> left, wp<TS<i64>> right) { return add(left, right); });

    const auto tested = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> left,
                            wp<TSD<i64, TSD<i64, TS<i64>>>> right) {
        const auto outer = map(
            [&](auto left_item, auto right_item) {
                return map(combine, left_item, right_item, "__key_set_arg__"_n = 0)[i64{1}];
            },
            left, right, "__key_set_arg__"_n = 0);
        return outer[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, {{1, 10}}}}, {{1, {{1, 11}}}}, {{1, {{1, 12}}}}},
        {{}, {{1, {{1, 20}}}}, {{1, {{1, literal::REMOVE}}}}});

    REQUIRE(actual.size() == 3);
    CHECK_FALSE(actual[0]);
    REQUIRE(actual[1]);
    CHECK(scalar_cast<i64>(actual[1]->view()).value() == 31);
    CHECK_FALSE(actual[2]);
}

TEST_CASE("map uses an explicitly selected TSS key set for a source graph",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:nested][guarantee:const]") {
    const auto tested = [&](wp<TSS<i64>> other_keys, wp<TSS<i64>> selected_keys) {
        const auto result = map(
            [](auto, auto key) { return key; }, other_keys, selected_keys,
            "__key_arg__"_n = 1, "__key_set_arg__"_n = 1);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{2}}, {{1}});

    CHECK(actual.equals<i64>({1}));
}

TEST_CASE("a pass-through TSD remains one input to every mapped graph",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:update]") {
    const auto add_factor = graph(
        "name"_n = "add_factor",
        [](wp<TS<i64>> value, wp<TSD<i64, TS<i64>>> factor_by_key) {
            return add_values(value, factor_by_key[i64{9}]);
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                            wp<TSD<i64, TS<i64>>> factors) {
        const auto result = map(add_factor, values, pass_through(factors));
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 2}}, {{1, 3}}, {{1, 4}}}, {{{9, 10}}, {{9, 20}}, {{9, 30}}});

    CHECK(actual.equals<i64>({12, 23, 34}));
}

TEST_CASE("map passes direct and reference TSD inputs through to every mapped graph",
          "[feature:map][feature:nested-graph][feature:operator][feature:reference][series:tsd]"
          "[shape:doubly-nested][transition:update][binding:reference-to-reference]") {
    const auto add_factor = graph(
        "name"_n = "add_direct_and_reference_factors",
        [](wp<TS<i64>> value, wp<TSD<i64, TS<i64>>> direct_factors,
           wp<TSD<i64, TS<i64>>> reference_factors) {
            const auto direct = direct_factors[i64{9}];
            const auto reference = reference_factors[i64{9}];
            return add_values(add_values(value, direct), reference);
        });

    const auto factors = node([](i64 factor, Out<TSD<i64, TS<i64>>> output) {
        output[9].set(factor);
    });

    const auto make_factors = graph("name"_n = "make_factors", [&](wp<TS<i64>> factor) {
        return factors(factor);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                            wp<TSD<i64, TS<i64>>> direct_factors,
                            wp<TS<i64>> reference_factor, wp<TS<bool>> run) {
        const auto reference = nested(make_factors, run, reference_factor);
        const auto result = map(add_factor, values, pass_through(direct_factors),
                                pass_through(reference));
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 2}}, {{1, 3}}, {{1, 4}}},
                                            {{{9, 10}}, {{9, 20}}, {{9, 30}}},
                                            {100, 200, 300},
                                            {true, true, true});

    CHECK(actual.equals<i64>({112, 223, 334}));
}

struct RetainedMappedSum {
    In<TS<i64, G>, optional> first;
    In<TS<i64, G>> second;
    Out<TS<i64>> output;

    void start() const noexcept { ++starts; }

    void evaluate() const { output.set((first.valid() ? *first : 0) + *second); }

    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a mapped inner graph survives one TSD item removal while another TSD retains its key",
          "[feature:map][feature:nested-graph][feature:lifecycle][feature:reference][series:tsd]"
          "[transition:removal][transition:resurrection]") {
    const auto sum = node<RetainedMappedSum>();

    const auto branch = graph(
        "name"_n = "sum_retained_tsd_items",
        [&](wp<TS<i64, G>, optional> first, wp<TS<i64, G>> second) {
            return sum(first, second);
        });

    const auto final_items = node([](In<TSD<i64, TS<i64>>> input,
                                     Out<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>> output) {
        input.for_each_removed([&](const auto &key) { output.erase(key); });
        input.for_each_added([&](const auto &key) { output[key].set(*input[key]); });
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second) {
        return map(branch, final_items(first), second)[i64{7}];
    };
    RetainedMappedSum::starts = 0;
    RetainedMappedSum::stops = 0;

    const auto actual = bg::run_graph(tested,
                                            {{{7, 10}}, {{7, literal::REMOVE}}, {{7, 30}}},
                                            {{{7, 1}}, {{7, 1}}, {{7, 1}}});

    CHECK(actual.equals<i64>({11, 1, 31}));
    CHECK(RetainedMappedSum::starts == 1);
    CHECK(RetainedMappedSum::stops == 1);
}

TEST_CASE("map forwards optional positional and named inputs only to optional branch parameters",
          "[feature:map][feature:nested-graph][feature:operator][feature:wiring][series:tsd]"
          "[validity:optional][definition:function]") {
    const auto dictionaries = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto values = node([](Out<TS<i64>>) {});

    const auto optional_positional = graph(
        "name"_n = "optional_positional_map_branch",
        [](wp<TS<i64>> item, wp<TS<i64>, optional>) { return item; });

    const auto required_positional = graph(
        "name"_n = "required_positional_map_branch",
        [](wp<TS<i64>> item, wp<TS<i64>>) { return item; });

    const auto optional_named = graph(
        "name"_n = "optional_named_map_branch",
        [](wp<TS<i64>, "item"_n> item, wp<TS<i64>, "context"_n, optional>) { return item; });

    const auto required_named = graph(
        "name"_n = "required_named_map_branch",
        [](wp<TS<i64>, "item"_n> item, wp<TS<i64>, "context"_n>) { return item; });

    testing::test_wiring([&] {
        const auto dictionary = dictionaries();
        const auto context = values();

        CHECK_NOTHROW(map(optional_positional, dictionary, opt(context)));
        CHECK_THROWS_AS(map(required_positional, dictionary, opt(context)), TypeResolutionError);
        CHECK_NOTHROW(map(optional_named, "item"_n = dictionary, "context"_n = opt(context)));
        CHECK_THROWS_AS(map(required_named, "item"_n = dictionary, "context"_n = opt(context)),
                        TypeResolutionError);
    });
}

TEST_CASE("map selects a key parameter by name",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[guarantee:const][definition:aggregate]") {
    struct NamedMapKey {
        wp<TS<i64>> value;
        wp<TS<i64, TimeSeriesGuarantee::const_>> selected_key;

        auto wire() const { return selected_key; }
    };

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto result = map(graph<NamedMapKey>(), "value"_n = values,
                                "__key_arg__"_n = "selected_key");
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 3}}, {{1, 8}}});

    CHECK(actual.equals<i64>({1, std::nullopt}));
}

TEST_CASE("map reads a TSD through a nested graph reference",
          "[feature:map][feature:nested-graph][feature:operator][series:tsd][series:reference]"
          "[shape:doubly-nested][binding:reference-to-reference]") {
    const auto build_values = graph(
        "name"_n = "build_values",
        [](wp<TS<i64>> value) { return values_at_key_one(value); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        const auto nested_values = nested(build_values, run, value);
        const auto result = map(
            [](auto item) { return increment(item); }, nested_values);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {2, 5}, {true, true});

    CHECK(actual.equals<i64>({3, 6}));
}

TEST_CASE("map reads a TSS through a nested graph reference",
          "[feature:map][feature:nested-graph][feature:operator][series:tss][series:reference]"
          "[shape:doubly-nested][binding:reference-to-value][guarantee:const]") {
    const auto keys = node([](bool condition, Out<TSS<i64>> output) {
        if (condition) {
            output.insert(1);
        } else {
            output.erase(1);
        }
    });

    const auto build_keys = graph("name"_n = "build_keys", [&](wp<TS<bool>> present) {
        return keys(present);
    });

    const auto tested = [&](wp<TS<bool>> present, wp<TS<bool>> run) {
        const auto nested_keys = nested(build_keys, run, present);
        const auto result = map(
            [](auto key) { return key; }, nested_keys, "__key_arg__"_n = 0);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {true, false, true}, {true, true, true});

    CHECK(actual.equals<i64>({1, std::nullopt, 1}));
}

TEST_CASE("map reads values using a nested TSS reference as its key set",
          "[feature:map][feature:nested-graph][feature:reference][series:tss][series:tsd]"
          "[shape:doubly-nested][binding:reference-to-reference]") {
    const auto keys = node([](bool condition, Out<TSS<i64>> output) {
        if (condition) {
            output.insert(1);
        } else {
            output.erase(1);
        }
    });

    const auto build_keys = graph("name"_n = "build_generic_keys", [&](wp<TS<bool>> present) {
        return keys(present);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> present, wp<TS<bool>> run) {
        const auto reference_keys = nested(build_keys, run, present);
        const auto result = map([](auto, auto item) { return item; }, values, reference_keys,
                                "__key_arg__"_n = 0, "__key_set_arg__"_n = 1);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 10}}, {{1, 20}}, {{1, 30}}},
                                            {true, false, true}, {true, true, true});

    CHECK(actual.equals<i64>({10, std::nullopt, 30}));
}

const auto capture_dictionary = node([](In<TSD<i64, TS<i64>>> input,
                                       Out<TSD<i64, TS<i64>>> output) {
    output.copy_tick(input);
});

const auto capture_row_dictionary = node([](In<TSD<i64, TSB<MapReferenceRow>>> input,
                                           Out<TSD<i64, TSB<MapReferenceRow>>> output) {
    output.copy_tick(input);
});

TEST_CASE("map drops a removed reference item while a sibling follows its target",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][shape:doubly-nested]"
          "[transition:removal][transition:update][binding:reference-to-reference]") {
    const auto lookup = graph(
        "name"_n = "lookup_row",
        [](wp<TS<i64>> key, wp<TSD<i64, TSB<MapReferenceRow>>> rows) { return rows[key]; });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> lookups,
                            wp<TSD<i64, TSB<MapReferenceRow>>> rows) {
        return capture_row_dictionary(map(lookup, lookups, pass_through(rows)));
    };

    const auto actual = bg::run_graph(
        tested,
        {{{1, 9}, {2, 9}}, {{1, literal::REMOVE}}, literal::SKIP},
        {{{9, {{"value", 2}, {"label", "beta"}}}}, literal::SKIP,
         {{9, {{"value", 3}, {"label", "updated"}}}}});

    CHECK(actual.equals({{{1, {{"value", 2}, {"label", "beta"}}},
                          {2, {{"value", 2}, {"label", "beta"}}}},
                         {{1, literal::REMOVE}},
                         {{2, {{"value", 3}, {"label", "updated"}}}}}));
}

TEST_CASE("each mapped graph reads the context for its own key",
          "[feature:map][feature:nested-graph][feature:switch][feature:context-time-series][series:tsd]"
          "[shape:doubly-nested][transition:update][binding:reference-to-reference]") {
    const auto add_context = node(
        [](In<TS<i64>> value, In<TS<i64>, "offset"_n, context> offset) {
            return *value + *offset;
        });

    const auto add_with_context = graph(
        "name"_n = "add_mapped_context",
        [&](wp<TS<i64>> value, wp<TS<i64>> offset, wp<TS<bool>> use_context) {
            auto context_scope = with_context("offset"_n = offset);
            return switch_(
                use_context,
                cases{
                    case_(true, [&](auto input) { return add_context(input); }),
                    case_(false, [](auto input) { return input; }),
                },
                value);
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                            wp<TSD<i64, TS<i64>>> offsets,
                            wp<TSD<i64, TS<bool>>> selectors) {
        return capture_dictionary(map(add_with_context, values, offsets, selectors));
    };

    const auto actual = bg::run_graph(
        tested,
        {{{1, 10}, {2, 20}}, {{1, 15}}},
        {{{1, 1}, {2, 2}}, {{2, 3}}},
        {{{1, true}, {2, false}}, {{1, false}, {2, true}}});

    CHECK(actual.equals({{{1, 11}, {2, 20}}, {{1, 15}, {2, 23}}}));
}

TEST_CASE("map restores a removed key through a retained mapped reference",
          "[feature:map][feature:nested-graph][feature:reference][series:tsd][series:tss]") {
    const auto identity = graph("name"_n = "identity_value", [](wp<TS<i64>> value) { return value; });
    const auto select_value = graph("name"_n = "select_mapped_value",
                                    [](wp<TS<i64>> key, wp<TS<i64>> value) { return value; });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TSS<i64>> selected_keys) {
        const auto mapped_values = map(identity, values);
        const auto selected_values = map(select_value, mapped_values, selected_keys,
                                         "__key_arg__"_n = 0, "__key_set_arg__"_n = 1);
        return selected_values[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 10}}, literal::SKIP, literal::SKIP},
        {{1}, {literal::REMOVED(1)}, {1}});

    CHECK(actual.equals<i64>({10, std::nullopt, 10}));
}

TEST_CASE("repro: map retargets a selected dictionary reference across a nested graph",
          "[.][repro][feature:map][feature:nested-graph][feature:reference][series:tsd]") {
    const auto select_dictionary = node(
        [](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
           In<REF<TSD<i64, TS<i64>>>> second, Out<REF<TSD<i64, TS<i64>>>> output) {
            output.copy(*use_second ? second : first);
        });

    const auto select = graph(
        "name"_n = "select_dictionary",
        [&](wp<TS<bool>> use_second, wp<TSD<i64, TS<i64>>> first,
            wp<TSD<i64, TS<i64>>> second) {
            return select_dictionary(use_second, first, second).as<TSD<i64, TS<i64>>>();
        });

    const auto tested = [&](wp<TS<bool>> use_second, wp<TSD<i64, TS<i64>>> first,
                            wp<TSD<i64, TS<i64>>> second, wp<TS<bool>> run) {
        const auto selected = nested(select, run, use_second, first, second);
        return capture_dictionary(map([](auto value) { return value; }, selected));
    };

    const auto actual = bg::run_graph(
        tested,
        {false, true, false, literal::SKIP},
        {{{1, 1}, {2, 2}}, literal::SKIP, {{1, 3}}, {{1, 4}}},
        {{{1, 10}, {2, 20}}, {{1, 11}}, literal::SKIP, literal::SKIP},
        {true, true, true, true});

    auto observed = std::vector<std::string>{};
    for (auto index = std::size_t{}; index < actual.size(); ++index) {
        actual.visit_tick(index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
            observed.push_back(fmt::format("{}", tick(type, valid, reader)));
        });
    }
    CAPTURE(observed);

    CHECK(actual.equals({{{1, 1}, {2, 2}}, {{1, 11}, {2, 20}}, {{1, 3}, {2, 2}}, {{1, 4}}}));
}

} // namespace
