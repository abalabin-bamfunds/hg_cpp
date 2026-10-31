#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_storage.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("all_valid checks every item in a fixed list",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][feature:collection-change]"
          "[transition:invalidation][transition:revalidation]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>, all_valid> input) {
        return *input[0] + *input[1];
    });
    const auto actual = bg::run_graph(sum, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}});

    CHECK(actual.equals({11, literal::INVALID, 13}));
}

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

template <template <typename, auto...> typename Field> struct ValiditySwapBundle {
    Field<i64> first;
    Field<i64> second;
};

struct CollectionValidityObservation {
    bool valid;
    bool became_valid;
    bool became_invalid;
    bool all_valid;

    bool operator==(const CollectionValidityObservation &) const = default;
};

} // namespace

TEST_CASE("owned collections retain cycle-start validity across item swaps",
          "[evaluation][validity][feature:validity][series:fixed-tsl][series:tsb]"
          "[feature:collection-change][transition:invalidation][transition:revalidation]") {
    auto list_observations = std::vector<CollectionValidityObservation>{};
    const auto write_list = node([](In<TS<i64>> step, Out<TSL<TS<i64>, 2>> output) {
        if (*step == 0) {
            output[0].set(10);
        } else {
            output[0].invalidate();
            output[1].set(20);
        }
    });
    const auto observe_list = node([&](In<TSL<TS<i64>, 2>, optional> input) {
        list_observations.push_back(
            {input.valid(), input.became_valid(), input.became_invalid(), input.all_valid()});
    });
    const auto list_graph = [&](wp<TS<i64>> step) { observe_list(write_list(step)); };

    auto bundle_observations = std::vector<CollectionValidityObservation>{};
    const auto write_bundle = node([](In<TS<i64>> step, Out<TSB<ValiditySwapBundle>> output) {
        if (*step == 0) {
            output->first.set(10);
        } else {
            output->first.invalidate();
            output->second.set(20);
        }
    });
    const auto observe_bundle = node([&](In<TSB<ValiditySwapBundle>, optional> input) {
        bundle_observations.push_back(
            {input.valid(), input.became_valid(), input.became_invalid(), input.all_valid()});
    });
    const auto bundle_graph = [&](wp<TS<i64>> step) { observe_bundle(write_bundle(step)); };

    auto copied_list_observations = std::vector<CollectionValidityObservation>{};
    const auto copy_list = node([](In<TSL<TS<i64>, 2>> input, Out<TSL<TS<i64>, 2>> output) {
        output.copy(input);
    });
    const auto observe_copied_list = node([&](In<TSL<TS<i64>, 2>, optional> input) {
        copied_list_observations.push_back(
            {input.valid(), input.became_valid(), input.became_invalid(), input.all_valid()});
    });
    const auto copied_list_graph = [&](wp<TSL<TS<i64>, 2>> input) { observe_copied_list(copy_list(input)); };

    auto copied_bundle_observations = std::vector<CollectionValidityObservation>{};
    const auto copy_bundle = node([](In<TSB<ValiditySwapBundle>> input, Out<TSB<ValiditySwapBundle>> output) {
        output.copy(input);
    });
    const auto observe_copied_bundle = node([&](In<TSB<ValiditySwapBundle>, optional> input) {
        copied_bundle_observations.push_back(
            {input.valid(), input.became_valid(), input.became_invalid(), input.all_valid()});
    });
    const auto copied_bundle_graph = [&](wp<TSB<ValiditySwapBundle>> input) {
        observe_copied_bundle(copy_bundle(input));
    };

    bg::run_graph(list_graph, std::array{i64{0}, i64{1}});
    bg::run_graph(bundle_graph, std::array{i64{0}, i64{1}});
    bg::run_graph(copied_list_graph, {{{0, 10}}, {{0, literal::INVALID}, {1, 20}}});
    bg::run_graph(copied_bundle_graph, {{{"first", 10}}, {{"first", literal::INVALID}, {"second", 20}}});

    REQUIRE(list_observations.size() == 2);
    CHECK((list_observations[0] == CollectionValidityObservation{true, true, false, false}));
    CHECK((list_observations[1] == CollectionValidityObservation{true, false, false, false}));

    REQUIRE(bundle_observations.size() == 2);
    CHECK((bundle_observations[0] == CollectionValidityObservation{true, true, false, false}));
    CHECK((bundle_observations[1] == CollectionValidityObservation{true, false, false, false}));

    REQUIRE(copied_list_observations.size() == 2);
    CHECK((copied_list_observations[0] == CollectionValidityObservation{true, true, false, false}));
    CHECK((copied_list_observations[1] == CollectionValidityObservation{true, false, false, false}));

    REQUIRE(copied_bundle_observations.size() == 2);
    CHECK((copied_bundle_observations[0] == CollectionValidityObservation{true, true, false, false}));
    CHECK((copied_bundle_observations[1] == CollectionValidityObservation{true, false, false, false}));
}

TEST_CASE("retained output views publish the child's validity at mutation time",
          "[evaluation][validity][feature:validity][series:fixed-tsl][feature:collection-change]"
          "[transition:invalidation][transition:revalidation]") {
    auto validity_during_write = std::vector<bool>{};
    auto observations = std::vector<CollectionValidityObservation>{};
    const auto write = node([&](In<TS<i64>> step, Out<TSL<TS<i64>, 1>> output) {
        const auto first = output[0];
        const auto second = output[0];

        if (*step == 0) {
            first.set(10);
            second.set(20);
            validity_during_write.push_back(output.valid());
        } else {
            first.invalidate();
            validity_during_write.push_back(output.valid());
            second.invalidate();
            validity_during_write.push_back(output.valid());
        }
    });
    const auto observe = node([&](In<TSL<TS<i64>, 1>, optional> input) {
        observations.push_back({input.valid(), input.became_valid(), input.became_invalid(), input.all_valid()});
    });
    const auto graph = [&](wp<TS<i64>> step) { observe(write(step)); };

    bg::run_graph(graph, std::array{i64{0}, i64{1}});

    CHECK(validity_during_write == std::vector<bool>{true, false, false});
    REQUIRE(observations.size() == 2);
    CHECK((observations[0] == CollectionValidityObservation{true, true, false, true}));
    CHECK((observations[1] == CollectionValidityObservation{false, false, true, false}));
}

TEST_CASE("nested owned collections track first and last valid children",
          "[evaluation][validity][feature:validity][series:fixed-tsl][feature:collection-change]"
          "[transition:first-tick][transition:invalidation][transition:revalidation]") {
    struct NestedObservation {
        CollectionValidityObservation outer;
        CollectionValidityObservation inner;
    };

    auto observations = std::vector<NestedObservation>{};
    const auto write = node([](In<TS<i64>> step, Out<TSL<TSL<TS<i64>, 2>, 1>> output) {
        if (*step == 0) {
            output[0][0].set(10);
        } else if (*step == 1) {
            output[0][0].invalidate();
            output[0][1].set(20);
        } else {
            output[0][1].invalidate();
        }
    });
    const auto observe = node([&](In<TSL<TSL<TS<i64>, 2>, 1>, optional> input) {
        const auto &inner = input[0];
        observations.push_back({{input.valid(), input.became_valid(), input.became_invalid(), input.all_valid()},
                                 {inner.valid(), inner.became_valid(), inner.became_invalid(), inner.all_valid()}});
    });
    const auto graph = [&](wp<TS<i64>> step) { observe(write(step)); };

    bg::run_graph(graph, std::array{i64{0}, i64{1}, i64{2}});

    REQUIRE(observations.size() == 3);
    CHECK(observations[0].outer == CollectionValidityObservation{true, true, false, true});
    CHECK(observations[0].inner == CollectionValidityObservation{true, true, false, false});
    CHECK(observations[1].outer == CollectionValidityObservation{true, false, false, true});
    CHECK(observations[1].inner == CollectionValidityObservation{true, false, false, false});
    CHECK(observations[2].outer == CollectionValidityObservation{false, false, true, false});
    CHECK(observations[2].inner == CollectionValidityObservation{false, false, true, false});
}

TEST_CASE("all_valid rejects an unpopulated fixed-list position",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[feature:collection-change]") {
    const auto query = node([](In<TSL<TS<i64>, 3>> input) { return input.all_valid(); });

    const auto gate = node([](In<TSL<TS<i64>, 3>, all_valid> input) {
        return *input[0] + *input[2];
    });
    CHECK(bg::run_graph(query, {{{0, 10}, {2, 20}}}).equals({false}));
    CHECK(bg::run_graph(gate, {{{0, 10}, {2, 20}}}).equals({literal::SKIP}));
}

TEST_CASE("an erased all_valid input uses the resolved runtime type",
          "[evaluation][validity][feature:validity][validity:all-valid][declaration:generic]"
          "[series:fixed-tsl][transition:invalidation][transition:revalidation]") {
    const auto query = node([](In<TS_T> input) { return input.all_valid(); });

    const auto gate = node([](In<TS_T, all_valid>) { return i64{1}; });

    const auto query_graph = [&](wp<TSL<TS<i64>, 2>> input) { return query(input); };

    const auto gate_graph = bg::graph("name"_n = "gate", [&](wp<TSL<TS<i64>, 2>> input) { return gate(input); });

    CHECK(bg::run_graph(query_graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}})
              .equals({true, false, true}));
    CHECK(bg::run_graph(gate_graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}})
              .equals({1, literal::INVALID, 1}));
}

TEST_CASE("all_valid treats a valid empty set as valid",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tss][transition:start]") {
    const auto size = node([](In<TSS<i64>, all_valid> input) { return static_cast<i64>(input.size()); });

    const auto query = node([](In<TSS<i64>> input) { return input.all_valid(); });

    CHECK(bg::run_graph(query, {{}}).equals({true}));
    CHECK(bg::run_graph(size, {{}}).equals<i64>({0}));
}

TEST_CASE("all_valid reports an invalid root even when the collection has no data",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][transition:start]") {
    const auto query = node(
        eval = [](In<TSL<TS<i64>, 2>, optional> input) { return input.all_valid(); },
        schedule_on_start = true);

    CHECK(bg::run_graph(query, {}).equals({false}));
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
    CHECK(bg::run_graph(sum, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, {{1, 12}}})
              .equals({11, literal::INVALID, 13}));
}

TEST_CASE("free TSL queries and enforces immediate item validity",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][free]"
          "[representation:free][transition:invalidation][transition:revalidation]") {
    const auto query = node([](In<TSL<TS<i64>, 2>> input) { return input.all_valid(); });

    const auto sum = node([](In<TSL<TS<i64>, 2>, all_valid> input) {
        return *input[0] + *input[1];
    });

    const auto sum_graph = bg::graph("name"_n = "sum_free_list", [&](wp<TS<i64>> first, wp<TS<i64>> changing) {
        return sum(tsl(first, changing));
    });

    const auto query_graph = [&](wp<TS<i64>> first, wp<TS<i64>> changing) {
        return query(tsl(first, changing));
    };

    CHECK(bg::run_graph(query_graph, {10, literal::SKIP, literal::SKIP}, {1, literal::INVALID, 3})
              .equals({true, false, true}));
    CHECK(bg::run_graph(sum_graph, {10, literal::SKIP, literal::SKIP}, {1, literal::INVALID, 3})
              .equals({11, literal::INVALID, 13}));
}

TEST_CASE("all_valid includes optional fields in a bundle",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[feature:collection-change]"
          "[transition:first-tick][transition:revalidation]") {
    const auto sum = node([](In<TSB<OptionalFieldBundle>, all_valid> input) {
        return *input->first + *input->second;
    });
    const auto actual = bg::run_graph(sum, {{{"first", 1}}, {{"second", 2}}});

    CHECK(actual.equals({literal::SKIP, 3}));
}

TEST_CASE("free TSB queries and enforces immediate field validity",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][free]"
          "[representation:free][transition:invalidation][transition:revalidation]") {
    const auto query = node([](In<TSB<OptionalFieldBundle>> input) { return input.all_valid(); });

    const auto sum = node([](In<TSB<OptionalFieldBundle>, all_valid> input) {
        return *input->first + *input->second;
    });

    const auto query_graph = [&](wp<TS<i64>> first, wp<TS<i64>> changing) {
        return query(bundle("first"_n = first, "second"_n = changing));
    };

    const auto sum_graph = bg::graph("name"_n = "sum_free_bundle", [&](wp<TS<i64>> first, wp<TS<i64>> changing) {
        return sum(bundle("first"_n = first, "second"_n = changing));
    });

    CHECK(bg::run_graph(query_graph, {10, literal::SKIP, literal::SKIP}, {1, literal::INVALID, 3})
              .equals({true, false, true}));
    CHECK(bg::run_graph(sum_graph, {10, literal::SKIP, literal::SKIP}, {1, literal::INVALID, 3})
              .equals({11, literal::INVALID, 13}));
}

TEST_CASE("all_valid uses the declared base schema for native derived bundle storage",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[binding:covariant][representation:native]"
          "[transition:invalidation][transition:revalidation]") {
    const auto inspect = node([](In<TSB<ValidityBaseBundle>, all_valid> input) {
        return *input->value;
    });

    const auto graph = bg::graph("name"_n = "inspect", [&](wp<TSB<ValidityDerivedBundle>> input) {
        return inspect(input);
    });

    CHECK(bg::run_graph(graph, {{{"value", 1}}, {{"value", literal::INVALID}}, {{"value", 2}}})
              .equals({1, literal::INVALID, 2}));
}

TEST_CASE("all_valid uses the declared base schema for free derived bundle fields",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[binding:covariant][representation:free][transition:invalidation][transition:revalidation]") {
    const auto inspect_base = node([](In<TSB<ValidityBaseBundle>> input) { return input.all_valid(); });

    const auto inspect_derived = node([](In<TSB<ValidityDerivedBundle>> input) { return input.all_valid(); });

    const auto inspect_actual = node([](In<TS_T> input) { return input.all_valid(); });

    const auto base_graph = [&](wp<TS<i64>> extra) {
        const auto input = bundle<TSB<ValidityDerivedBundle>>(
            "value"_n = operators::const_(i64{1}), "extra"_n = extra);
        return inspect_base(input);
    };

    const auto derived_graph = [&](wp<TS<i64>> extra) {
        const auto input = bundle<TSB<ValidityDerivedBundle>>(
            "value"_n = operators::const_(i64{1}), "extra"_n = extra);
        return inspect_derived(input);
    };

    const auto actual_graph = [&](wp<TS<i64>> extra) {
        const auto input = bundle<TSB<ValidityDerivedBundle>>(
            "value"_n = operators::const_(i64{1}), "extra"_n = extra);
        return inspect_actual(input);
    };

    CHECK(bg::run_graph(base_graph, {1, literal::INVALID, 3})
              .equals({true, std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(derived_graph, {1, literal::INVALID, 3}).equals({true, false, true}));
    CHECK(bg::run_graph(actual_graph, {1, literal::INVALID, 3}).equals({true, false, true}));
}

TEST_CASE("all_valid checks nested bundle roots without recursively checking their fields",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][schema:named]"
          "[binding:covariant][representation:native][feature:collection-change]") {
    const auto inspect = node([](In<TSB<NestedValidityBaseBundle>, all_valid> input) {
        return input.all_valid() && !input->nested.all_valid();
    });

    const auto graph = [&](wp<TSB<NestedValidityDerivedBundle>> input) { return inspect(input); };

    CHECK(bg::run_graph(graph, {{{"nested", {{"value", 10}}}}}).equals({true}));
}

TEST_CASE("all_valid follows a selected reference using the declared base schema",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][series:reference]"
          "[schema:named][binding:reference-to-value][representation:covariant]"
          "[transition:invalidation][transition:revalidation]") {
    struct select_bundle_reference {
        In<TS<i64>> choose_second;
        In<REF<TSB<ValidityDerivedBundle>>, passive, optional> first;
        In<REF<TSB<ValidityDerivedBundle>>, passive, optional> second;
        Out<REF<TSB<ValidityDerivedBundle>>> output;

        void evaluate() const { output.copy(*choose_second == 0 ? first : second); }
    };

    const auto changing_value = node([](i64 value) { return value; });

    const auto select = node<select_bundle_reference>();

    const auto inspect = node([](In<TSB<ValidityBaseBundle>, all_valid> input) { return *input->value; });

    const auto graph = [&](wp<TS<i64>> choose_second, wp<TSB<ValidityDerivedBundle>> first,
                           wp<TS<i64>> changing) {
        const auto second = bundle<TSB<ValidityDerivedBundle>>(
            "value"_n = changing_value(changing), "extra"_n = operators::const_(i64{2}));
        return inspect(select(choose_second, first, second));
    };

    CHECK(bg::run_graph(graph, ticks({0, 1, 1}), input<TSB<ValidityDerivedBundle>>({{{"value", 1}}}),
                        ticks({1, literal::INVALID, 3}))
              .equals({1, std::nullopt, 3}));
}

TEST_CASE("all_valid refreshes on a passive valid target switch",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][series:reference]"
          "[activity:passive][binding:reference-to-value][transition:update][feature:collection-change]") {
    const auto first = node([](Out<TSL<TS<i64>, 2>> output) {
        output[0].set(1);
        output[1].set(10);
    });

    const auto second = node([](Out<TSL<TS<i64>, 2>> output) {
        output[0].set(2);
        output[1].invalidate();
    });

    const auto select = node([](In<TS<i64>> choose_second, In<REF<TSL<TS<i64>, 2>>, passive> first,
                                In<REF<TSL<TS<i64>, 2>>, passive> second,
                                Out<REF<TSL<TS<i64>, 2>>> output) {
        output.copy(*choose_second == 0 ? first : second);
    });

    auto evaluations = 0;

    const auto gate = node([&](In<TSL<TS<i64>, 2>, passive, all_valid> input, In<TS<i64>> pulse) {
        ++evaluations;
        return *input[0] + *input[1] + *pulse;
    });

    const auto graph = [&](wp<TS<i64>> choose_second, wp<TS<i64>> pulse) -> wp<> {
        return wp<>{gate(select(choose_second, first(), second()), pulse)};
    };

    const auto actual =
        bg::run_graph(graph, {0, 1, 1}, {1, literal::SKIP, 2});

    CHECK(actual.equals({12, literal::INVALID, literal::SKIP}));
    CHECK(evaluations == 1);
}

TEST_CASE("all_valid ignores removed TSD items and accepts an empty live set",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][feature:collection-change]"
          "[transition:invalidation][transition:removal]") {
    const auto sum = node([](In<TS<i64>, all_valid> constant, In<TS<i64>> value, i64 adjustment,
                             In<TSD<i64, TS<i64>>, all_valid> input) {
        auto total = i64{};
        input.for_each_value([&](const auto &item_value) { total += *item_value; });
        return *constant + *value + adjustment + total;
    });

    const auto graph = [&](wp<TS<i64>> value, wp<TSD<i64, TS<i64>>> input) {
        return sum(operators::const_(i64{42}), value, i64{1}, input);
    };

    const auto actual = bg::run_graph(
        graph, {0, 0, 0, 0},
        {{{1, 10}, {2, 20}}, {{1, literal::INVALID}}, {{1, literal::REMOVE}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals({73, std::nullopt, 63, 43}));
}

TEST_CASE("all_valid descriptors stay aligned across collection inputs, constants, and scalars",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][series:tsd]"
          "[guarantee:const][transition:start]") {
    const auto sum = node([](In<TS<i64>, all_valid> constant, In<TSL<TS<i64>, 2>, all_valid> list,
                             In<TS<i64>> pulse, i64 adjustment, In<TSD<i64, TS<i64>>, all_valid> dictionary) {
        return *constant + *list[0] + *list[1] + *pulse + adjustment + *dictionary[1];
    });

    const auto graph = [&](wp<TS<i64>> pulse, wp<TSL<TS<i64>, 2>> list, wp<TSD<i64, TS<i64>>> dictionary) {
        return sum(operators::const_(i64{42}), list, pulse, i64{2}, dictionary);
    };

    CHECK(bg::run_graph(graph, {1}, {{{0, 10}, {1, 20}}}, {{{1, 30}}})
              .equals({105}));
}

TEST_CASE("all_valid accepts a constant leaf time series",
          "[evaluation][validity][feature:validity][validity:all-valid][series:value][feature:constant]") {
    const auto copy = node([](In<TS<i64>, all_valid> input) { return *input; });

    const auto graph = [&] { return copy(operators::const_(i64{42})); };

    CHECK(bg::run_graph(graph).equals({42}));
}

TEST_CASE("all_valid observes a defaulted input changing to its fallback",
          "[evaluation][validity][feature:validity][validity:all-valid][transition:invalidation]"
          "[transition:revalidation]") {
    const auto copy = node([](In<TS<i64>, all_valid> input) { return *input; });

    const auto graph = [&](wp<TS<i64>> source) { return copy(operators::default_(source, i64{0})); };

    CHECK(bg::run_graph(graph, {1, literal::INVALID, 3}).equals<i64>({1, 0, 3}));
}

TEST_CASE("all_valid ignores a retained but missing dictionary key",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:getitem][series:tsd]"
          "[series:reference]"
          "[transition:update]") {
    struct observation {
        i64 size;
        bool missing_valid;

        bool operator==(const observation &) const = default;
    };

    const auto inspect = node([](In<TSD<i64, TS<i64>>, all_valid> dictionary,
                                 In<REF<TS<i64>>, optional> missing) {
        return observation{static_cast<i64>(dictionary.size()), missing.valid()};
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> dictionary) {
        return inspect(dictionary, dictionary[2]);
    };

    CHECK(bg::run_graph(graph, {{{1, 10}}, {{1, 20}}})
              .equals<observation>({observation{1, false}, observation{1, false}}));
}

namespace {

struct changing_sampled_item_source {
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

TEST_CASE("all_valid checks sampled TSD item values as they invalidate and revalidate",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][series:reference]"
          "[feature:sampling][transition:invalidation][transition:revalidation]") {

    const auto target = node<changing_sampled_item_source>();

    const auto make_dictionary = node([](In<REF<TS<i64>>> item, Out<TSD<i64, REF<TS<i64>>>> output) {
        output[1].copy(item);
    });

    const auto sum = node([](In<TSD<i64, TS<i64>>, all_valid> input) { return *input[1]; });

    const auto report = node([](In<TS<i64>, optional> input) { return input.valid(); });

    const auto graph = [&] {
        const auto dictionary = make_dictionary(target());
        return report(sum(dictionary));
    };

    CHECK(bg::run_graph(graph).equals({true, false, true}));
}

TEST_CASE("all_valid checks live items in sampled native dictionaries",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][series:reference]"
          "[feature:sampling][transition:invalidation][transition:revalidation]") {
    struct observation {
        bool all_valid;
        i64 total;

        bool operator==(const observation &) const = default;
    };

    const auto select = node([](In<TS<i64>> choose_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second, Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[0].copy(*choose_second == 0 ? first : second);
    });

    const auto inspect = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input) {
        const auto inner = input[0];
        const auto inner_is_valid = inner.all_valid();
        const auto first_is_valid = inner[1].valid();
        const auto second_is_valid = inner[2].valid();
        const auto total = (first_is_valid ? *inner[1] : i64{}) + (second_is_valid ? *inner[2] : i64{});
        return observation{inner_is_valid, total};
    });

    const auto graph = bg::graph(
        "name"_n = "sampled_native_dictionary",
        [&](wp<TS<i64>> selection, wp<TSD<i64, TS<i64>>> first, wp<TSD<i64, TS<i64>>> second) {
        const auto selected = select(selection, first, second);
        return inspect(selected);
    });

    CHECK(bg::run_graph(
              graph, {0, 0, 1, 1}, {{{1, 10}, {2, 20}}},
              {{{1, 30}, {2, 40}}, literal::SKIP, {{2, literal::INVALID}}, {{2, 42}}})
              .equals<observation>({observation{true, 30}, std::nullopt, observation{false, 30},
                                    observation{true, 72}}));
}

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

    CHECK(bg::run_graph(graph, {{{0, 1}, {1, 10}},
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
    const auto inspect = node([](In<TSL<TSL<TS<i64>, 2>, 1>, all_valid> input) {
        return input[0].valid() && !input[0].all_valid();
    });

    const auto graph = [&](wp<TSL<TSL<TS<i64>, 2>, 1>> input) { return inspect(input); };

    CHECK(bg::run_graph(graph, {{{0, {{0, 10}}}}}).equals({true}));
}

TEST_CASE("all_valid on a reference checks the reference rather than its target items",
          "[evaluation][validity][feature:validity][validity:all-valid][series:reference]"
          "[series:fixed-tsl]") {
    const auto inspect = node([](In<REF<TSL<TS<i64>, 2>>, all_valid> input) { return input.valid(); });

    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) {
        return inspect(input);
    };

    CHECK(bg::run_graph(graph, {{{0, 10}}}).equals({true}));
}

TEST_CASE("a passive all_valid input checks items on scheduled evaluation attempts",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:input-activity][series:fixed-tsl]"
          "[activity:passive][transition:invalidation]") {
    const auto combine = node([](In<TSL<TS<i64>, 2>, passive, all_valid> input, In<TS<i64>> pulse) {
        return *input[0] + *input[1] + *pulse;
    });

    const auto observe = node([](In<TS<i64>, optional> input) { return input.valid(); });

    const auto graph = [&](wp<TS<i64>> pulse, wp<TSL<TS<i64>, 2>> input) {
        return observe(combine(input, pulse));
    };

    CHECK(bg::run_graph(
              graph, {100, literal::SKIP, 200, literal::SKIP, 300},
              {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, literal::SKIP, {{1, 12}}})
              .equals({true, std::nullopt, false, std::nullopt, true}));
}

TEST_CASE("dynamic activity defers all_valid item checks until a scheduled attempt",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:input-activity]"
          "[series:fixed-tsl][activity:active][transition:invalidation][transition:revalidation]") {
    const auto observe = node([](In<TSL<TS<i64>, 2>, all_valid, dynamic_activity> input, In<TS<i64>> pulse) {
        const auto value = *input[0] + *input[1] + *pulse;
        input.make_passive();
        return value;
    });

    const auto report = node([](In<TS<i64>, optional> input) { return input.valid(); });

    const auto graph = [&](wp<TS<i64>> pulse, wp<TSL<TS<i64>, 2>> input) {
        return report(observe(input, pulse));
    };

    CHECK(bg::run_graph(
              graph, {1, literal::SKIP, 2, literal::SKIP, 3},
              {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, literal::SKIP, {{1, 12}}})
              .equals({true, std::nullopt, false, std::nullopt, true}));
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

TEST_CASE("a timer evaluation checks passive all_valid items",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[activity:passive][feature:timed-scheduling][transition:invalidation][transition:revalidation]") {
    const auto gate = node<timed_all_valid_input_gate>();

    const auto observe = node([](In<TS<i64>, optional> input) { return input.valid(); });

    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return observe(gate(input)); };

    CHECK(bg::run_graph(graph, {{{0, 1}, {1, 10}}, {{1, literal::INVALID}}, literal::SKIP, {{1, 12}}})
              .equals({true, std::nullopt, false, std::nullopt, std::nullopt, true}));
}
