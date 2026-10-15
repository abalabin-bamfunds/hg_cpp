#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>

using namespace bg;
using namespace bg::literals;

namespace {

template <template <typename, auto...> typename Field> struct OptionalFieldBundle {
    Field<i64> first;
    Field<i64, optional> second;
};

template <template <typename, auto...> typename Field> struct ValidityBaseBundle {
    Field<i64> value;
};

template <template <typename, auto...> typename Field>
struct ValidityDerivedBundle : bundle_extends<ValidityBaseBundle, Field> {
    Field<i64> extra;
};

template <template <typename, auto...> typename Field> struct NestedValidityBaseBundle {
    Field<TSB<ValidityDerivedBundle>> nested;
};

template <template <typename, auto...> typename Field>
struct NestedValidityDerivedBundle : bundle_extends<NestedValidityBaseBundle, Field> {
    Field<i64> extra;
};

struct validity_bundle_reference_selector {
    In<TS<i64>> condition;
    In<REF<TSB<ValidityDerivedBundle>>, passive, optional> left;
    In<REF<TSB<ValidityDerivedBundle>>, passive, optional> right;
    Out<REF<TSB<ValidityDerivedBundle>>> output;

    void evaluate() const {
        if (*condition == 0) {
            output.copy(left);
        } else {
            output.copy(right);
        }
    }
};

struct invalidating_leaf_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if ((*step)++ == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.set(3);
    }
};

} // namespace

TEST_CASE("all_valid checks every item in a fixed list",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][feature:collection-change]"
          "[transition:invalidation][transition:revalidation]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? input.value() : i64{-1});
    });
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return observe(sum(input)); };

    const auto actual = testing::test_graph(graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}});

    CHECK(actual.equals({11, -1, 13}));
}

TEST_CASE("all_valid rejects an unpopulated fixed-list position",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[feature:collection-change]") {
    const auto query = node([](In<TSL<TS<i64>, 3>> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto gate = node([](In<TSL<TS<i64>, 3>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[2]);
    });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&](wp<TSL<TS<i64>, 3>> input) {
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph, {{{0, 10}, {2, 20}}}).equals({0}));
}

TEST_CASE("an erased all_valid input uses the resolved runtime type",
          "[evaluation][validity][feature:validity][validity:all-valid][declaration:generic]"
          "[series:fixed-tsl][transition:invalidation][transition:revalidation]") {
    const auto query = node([](In<TS_T> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto gate = node([](In<TS_T, all_valid>, Out<TS<i64>> output) { output.set(1); });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) {
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}})
              .equals({3, 0, 3}));
}

TEST_CASE("all_valid treats a valid empty set as valid",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tss][transition:start]") {
    const auto query = node([](In<TSS<i64>> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto gate = node([](In<TSS<i64>, all_valid> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&](wp<TSS<i64>> input) {
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph, {{}}).equals({3}));
}

TEST_CASE("all_valid reports an invalid root even when the collection has no data",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][transition:start]") {
    const auto query = node(
        eval = [](In<TSL<TS<i64>, 2>, optional> input, Out<TS<bool>> output) { output.set(input.all_valid()); },
        schedule_on_start = true);
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return query(input); };

    CHECK(testing::test_graph(graph, {}).equals({false}));
}

TEST_CASE("all_valid on an empty input view rejects invalid roots without runtime data",
          "[evaluation][validity][feature:validity][validity:all-valid][transition:start]"
          "[declaration:generic]") {
    const auto empty = TimeSeriesInputData{};
    const auto erased = In<TS_T>{empty};
    const auto list = In<TSL<TS_T>>{empty};
    const auto dictionary = In<TSD<i64, TS<i64>>>{empty};
    const auto bundle = In<TSB<SCHEMA>>{empty};

    CHECK_FALSE(erased.all_valid());
    CHECK_FALSE(list.all_valid());
    CHECK_FALSE(dictionary.all_valid());
    CHECK_FALSE(bundle.all_valid());
}

TEST_CASE("aggregate all_valid inputs gate evaluation until every list item is valid",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[definition:aggregate][transition:invalidation][transition:revalidation]") {
    struct sum_valid_list {
        In<TSL<TS<i64>, 2>, all_valid> input;
        Out<TS<i64>> output;

        void evaluate() const { output.set(*input[0] + *input[1]); }
    };

    const auto sum = node<sum_valid_list>();
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? input.value() : i64{-1});
    });
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return observe(sum(input)); };

    CHECK(testing::test_graph(graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}})
              .equals({11, -1, 13}));
}

TEST_CASE("free TSL queries and enforces immediate item validity",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][free]"
          "[representation:free][transition:invalidation][transition:revalidation]") {
    const auto changing_item = node<invalidating_leaf_source>();
    const auto query = node([](In<TSL<TS<i64>, 2>> input, Out<TS<bool>> output) {
        output.set(input.all_valid());
    });
    const auto gate = node([](In<TSL<TS<i64>, 2>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&](wp<TS<i64>> first) {
        const auto input = tsl(first, changing_item());
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 1>{10}).equals({3, 0, 3}));
}

TEST_CASE("all_valid includes optional fields in a bundle",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[feature:collection-change]"
          "[transition:first-tick][transition:revalidation]") {
    const auto sum = node([](In<TSB<OptionalFieldBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->first + *input->second);
    });
    const auto graph = [&](wp<TSB<OptionalFieldBundle>> input) { return sum(input); };

    const auto actual = testing::test_graph(
        graph, {{{"first", 1}}, {{"second", 2}}});

    CHECK(actual.equals({std::nullopt, 3}));
}

TEST_CASE("free TSB queries and enforces immediate field validity",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][free]"
          "[representation:free][transition:invalidation][transition:revalidation]") {
    const auto changing_field = node<invalidating_leaf_source>();
    const auto query = node([](In<TSB<OptionalFieldBundle>> input, Out<TS<bool>> output) {
        output.set(input.all_valid());
    });
    const auto gate = node([](In<TSB<OptionalFieldBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->first + *input->second);
    });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&](wp<TS<i64>> first) {
        const auto input = bundle("first"_n = first, "second"_n = changing_field());
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 1>{10}).equals({3, 0, 3}));
}

TEST_CASE("all_valid uses the declared base schema for native derived bundle storage",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[binding:covariant][representation:native]"
          "[transition:invalidation][transition:revalidation]") {
    const auto inspect = node([](In<TSB<ValidityBaseBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->value);
    });
    const auto graph = [&](wp<TSB<ValidityDerivedBundle>> input) { return inspect(input); };

    CHECK(testing::test_graph(graph, {{{"value", 1}}, {{"value", literal::INVALID}}, {{"value", 2}}})
              .equals({1, std::nullopt, 2}));
}

TEST_CASE("all_valid uses the declared base schema for free derived bundle fields",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[binding:covariant][representation:free][transition:invalidation][transition:revalidation]") {
    const auto extra = node<invalidating_leaf_source>();
    const auto inspect_base = node([](In<TSB<ValidityBaseBundle>> input, Out<TS<bool>> output) {
        output.set(input.all_valid());
    });
    const auto inspect_derived = node([](In<TSB<ValidityDerivedBundle>> input, Out<TS<bool>> output) {
        output.set(input.all_valid());
    });
    const auto inspect_actual = node([](In<TS_T> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto report = node([](In<TS<bool>> base, In<TS<bool>> derived, In<TS<bool>> actual, Out<TS<i64>> output) {
        output.set(static_cast<i64>(base.value()) * 4 + static_cast<i64>(derived.value()) * 2 +
                   static_cast<i64>(actual.value()));
    });
    const auto graph = [&] {
        const auto input = bundle<TSB<ValidityDerivedBundle>>(
            "value"_n = operators::const_(i64{1}), "extra"_n = extra());
        return report(inspect_base(input), inspect_derived(input), inspect_actual(input));
    };

    CHECK(testing::test_graph(graph).equals({7, 4, 7}));
}

TEST_CASE("all_valid checks nested bundle roots without recursively checking their fields",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[binding:covariant][representation:native][feature:collection-change]") {
    const auto inspect = node([](In<TSB<NestedValidityBaseBundle>, all_valid> input, Out<TS<bool>> output) {
        output.set(input.all_valid() && !input->nested.all_valid());
    });
    const auto graph = [&](wp<TSB<NestedValidityDerivedBundle>> input) { return inspect(input); };

    CHECK(testing::test_graph(graph, {{{"nested", {{"value", 10}}}}}).equals({true}));
}

TEST_CASE("all_valid follows a selected reference using the declared base schema",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][series:reference]"
          "[schema:named][binding:reference-to-value][representation:covariant]"
          "[transition:invalidation][transition:revalidation]") {
    const auto second_value = node<invalidating_leaf_source>();
    const auto second = [&] {
        return bundle<TSB<ValidityDerivedBundle>>("value"_n = second_value(),
                                                  "extra"_n = operators::const_(i64{2}));
    };
    const auto select = node<validity_bundle_reference_selector>();
    const auto inspect = node([](In<TSB<ValidityBaseBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->value);
    });
    const auto graph = [&](wp<TS<i64>> choose_second, wp<TSB<ValidityDerivedBundle>> first) {
        return inspect(select(choose_second, first.as<REF<TSB<ValidityDerivedBundle>>>(),
                              second().as<REF<TSB<ValidityDerivedBundle>>>()));
    };

    CHECK(testing::test_graph(graph, {0, 1, 1}, {{{"value", 1}}})
              .equals({1, std::nullopt, 3}));
}

TEST_CASE("all_valid ignores removed TSD items and accepts an empty live set",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][feature:collection-change]"
          "[transition:invalidation][transition:removal]") {
    const auto sum = node([](In<TS<i64>, all_valid> constant, In<TS<i64>> value, i64 adjustment,
                             In<TSD<i64, TS<i64>>, all_valid> input, Out<TS<i64>> output) {
        auto total = i64{};
        input.for_each_value([&](const auto &item_value) { total += *item_value; });
        output.set(*constant + *value + adjustment + total);
    });
    const auto graph = [&](wp<TS<i64>> value, wp<TSD<i64, TS<i64>>> input) {
        return sum(operators::const_(i64{42}), value, i64{1}, input);
    };

    const auto actual = testing::test_graph(graph, {0, 0, 0, 0}, {{{1, 10}, {2, 20}},
                                                                  {{1, literal::INVALID}},
                                                                  {{1, literal::REMOVE}},
                                                                  {{2, literal::REMOVE}}});

    CHECK(actual.equals({73, std::nullopt, 63, 43}));
}

TEST_CASE("all_valid descriptors stay aligned across collection inputs, constants, and scalars",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][series:tsd]"
          "[guarantee:const][transition:start]") {
    const auto sum = node([](In<TS<i64>, all_valid> constant, In<TSL<TS<i64>, 2>, all_valid> list,
                             In<TS<i64>> pulse, i64 adjustment, In<TSD<i64, TS<i64>>, all_valid> dictionary,
                             Out<TS<i64>> output) {
        output.set(*constant + *list[0] + *list[1] + *pulse + adjustment + *dictionary[1]);
    });
    const auto graph = [&](wp<TS<i64>> pulse, wp<TSL<TS<i64>, 2>> list, wp<TSD<i64, TS<i64>>> dictionary) {
        return sum(operators::const_(i64{42}), list, pulse, i64{2}, dictionary);
    };

    CHECK(testing::test_graph(graph, {1}, {{{0, 10}, {1, 20}}}, {{{1, 30}}})
              .equals({105}));
}

TEST_CASE("all_valid accepts a constant leaf time series",
          "[evaluation][validity][feature:validity][validity:all-valid][series:value][feature:constant]") {
    const auto copy = node([](In<TS<i64>, all_valid> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&] { return copy(operators::const_(i64{42})); };

    CHECK(testing::test_graph(graph).equals({42}));
}

TEST_CASE("all_valid observes a defaulted input changing to its fallback",
          "[evaluation][validity][feature:validity][validity:all-valid][transition:invalidation]"
          "[transition:revalidation]") {
    const auto source = node<invalidating_leaf_source>();
    const auto copy = node([](In<TS<i64>, all_valid> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&] { return copy(operators::default_(source(), i64{0})); };

    CHECK(testing::test_graph(graph).equals({1, 0, 3}));
}

TEST_CASE("all_valid ignores a retained but missing dictionary key",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:getitem][series:tsd]"
          "[series:reference]"
          "[transition:update]") {
    const auto inspect = node([](In<TSD<i64, TS<i64>>, all_valid> dictionary,
                                 In<REF<TS<i64>>, optional> missing, Out<TS<i64>> output) {
        output.set(static_cast<i64>(dictionary.size() * 10) + static_cast<i64>(missing.valid()));
    });
    const auto graph = [&](wp<TSD<i64, TS<i64>>> dictionary) {
        return inspect(dictionary, dictionary[2]);
    };

    CHECK(testing::test_graph(graph, {{{1, 10}}, {{1, 20}}}).equals({10, 10}));
}

TEST_CASE("all_valid checks sampled TSD item values as they invalidate and revalidate",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][series:reference]"
          "[feature:sampling][transition:invalidation][transition:revalidation]") {
    const auto target = node<invalidating_leaf_source>();
    const auto make_dictionary = node([](In<REF<TS<i64>>> item, Out<TSD<i64, REF<TS<i64>>>> output) {
        output[1].copy(item);
    });
    const auto sum = node([](In<TSD<i64, TS<i64>>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input[1]);
    });
    const auto report = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&] {
        const auto dictionary = make_dictionary(target().as<REF<TS<i64>>>());
        return report(sum(dictionary));
    };

    CHECK(testing::test_graph(graph).equals({true, false, true}));
}

TEST_CASE("all_valid checks live items in sampled native dictionaries",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][series:reference]"
          "[feature:sampling][transition:invalidation][transition:revalidation]") {
    const auto select = node([](In<TS<i64>> choose_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second, Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[0].copy(*choose_second == 0 ? first : second);
    });
    const auto inspect = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto inner = input[0];
        const auto inner_is_valid = inner.all_valid();
        const auto first_is_valid = inner[1].valid();
        const auto second_is_valid = inner[2].valid();
        const auto total = (first_is_valid ? *inner[1] : i64{}) + (second_is_valid ? *inner[2] : i64{});
        const auto value = (inner_is_valid ? i64{1000} : i64{}) + total;
        output.set(value * 10 + static_cast<i64>(inner_is_valid));
    });
    const auto graph = [&](wp<TS<i64>> selection, wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second) {
        const auto selected = select(selection, first.as<REF<TSD<i64, TS<i64>>>>(),
                                     second.as<REF<TSD<i64, TS<i64>>>>());
        return inspect(selected);
    };

    CHECK(testing::test_graph(graph, {0, 0, 1, 1}, {{{1, 10}, {2, 20}}},
                              {{{1, 30}, {2, 40}}, literal::SKIP, {{2, literal::INVALID}}, {{2, 42}}})
              .equals({10301, std::nullopt, 300, 10721}));
}

struct timed_all_valid_input_gate {
    static constexpr bool uses_timed_scheduling = true;

    In<TSL<TS<i64>, 2>, passive, all_valid> input;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(*input[0] + *input[1]);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{5});
    }

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

TEST_CASE("an all_valid invalidation handler runs once per failed transition",
          "[evaluation][validity][feature:validity][validity:all-valid][transition:invalidation]"
          "[transition:revalidation][series:fixed-tsl]") {
    struct invalidation_counting_node {
        In<TSL<TS<i64>, 2>, all_valid> input;
        State<i64> invalidations;
        Out<TS<i64>> output;

        void evaluate() const { output.set(1000 + *invalidations); }

        void on_invalidation() const {
            ++*invalidations;
            output.set(*invalidations);
        }
    };

    const auto observe = node<invalidation_counting_node>();
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return observe(input); };

    CHECK(testing::test_graph(graph, {{{0, 1}, {1, 10}},
                                      {{1, literal::INVALID}},
                                      {{0, 2}},
                                      {{1, 12}},
                                      {{0, literal::INVALID}},
                                      {{1, 13}}})
              .equals({1000, 1, std::nullopt, 1001, 2, std::nullopt}));
}

TEST_CASE("all_valid checks immediate nested items without recursively checking them",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[feature:collection-change]") {
    const auto inspect = node([](In<TSL<TSL<TS<i64>, 2>, 1>, all_valid> input, Out<TS<bool>> output) {
        output.set(input[0].valid() && !input[0].all_valid());
    });
    const auto graph = [&](wp<TSL<TSL<TS<i64>, 2>, 1>> input) { return inspect(input); };

    CHECK(testing::test_graph(graph, {{{0, {{0, 10}}}}}).equals({true}));
}

TEST_CASE("all_valid on a reference checks the reference rather than its target items",
          "[evaluation][validity][feature:validity][validity:all-valid][series:reference]"
          "[series:fixed-tsl]") {
    const auto inspect = node([](In<REF<TSL<TS<i64>, 2>>, all_valid> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) {
        return inspect(input.as<REF<TSL<TS<i64>, 2>>>());
    };

    CHECK(testing::test_graph(graph, {{{0, 10}}}).equals({true}));
}

TEST_CASE("a passive all_valid input checks items on scheduled evaluation attempts",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:input-activity][series:fixed-tsl]"
          "[activity:passive][transition:invalidation]") {
    const auto combine = node([](In<TSL<TS<i64>, 2>, passive, all_valid> input, In<TS<i64>> pulse,
                                 Out<TS<i64>> output) { output.set(*input[0] + *input[1] + *pulse); });
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](wp<TS<i64>> pulse, wp<TSL<TS<i64>, 2>> input) {
        return observe(combine(input, pulse));
    };

    CHECK(testing::test_graph(graph, {100, literal::SKIP, 200, literal::SKIP, 300},
                              {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, literal::SKIP, {{1, 12}}})
              .equals({true, std::nullopt, false, std::nullopt, true}));
}

TEST_CASE("dynamic activity defers all_valid item checks until a scheduled attempt",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:input-activity]"
          "[series:fixed-tsl][activity:active][transition:invalidation][transition:revalidation]") {
    const auto observe = node([](In<TSL<TS<i64>, 2>, all_valid, dynamic_activity> input, In<TS<i64>> pulse,
                                 Out<TS<i64>> output) {
        output.set(*input[0] + *input[1] + *pulse);
        input.make_passive();
    });
    const auto report = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](wp<TS<i64>> pulse, wp<TSL<TS<i64>, 2>> input) {
        return report(observe(input, pulse));
    };

    CHECK(testing::test_graph(graph, {1, literal::SKIP, 2, literal::SKIP, 3},
                              {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, literal::SKIP, {{1, 12}}})
              .equals({true, std::nullopt, false, std::nullopt, true}));
}

TEST_CASE("a timer evaluation checks passive all_valid items",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[activity:passive][feature:timed-scheduling][transition:invalidation][transition:revalidation]") {
    const auto gate = node<timed_all_valid_input_gate>();
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return observe(gate(input)); };

    CHECK(testing::test_graph(graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, literal::SKIP, {{1, 12}}})
              .equals({true, std::nullopt, false, std::nullopt, std::nullopt, true}));
}
