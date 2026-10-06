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

struct validity_derived_bundle_source {
    i64 value;
    Out<TSB<ValidityDerivedBundle>> output;

    void start() const { output->value.set(value); }
};

struct changing_derived_bundle_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSB<ValidityDerivedBundle>> output;

    void start(EvaluationContext *context) const {
        output->value.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if ((*step)++ == 0) {
            output->value.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output->value.set(2);
    }
};

struct nested_derived_bundle_source {
    Out<TSB<NestedValidityDerivedBundle>> output;

    void start() const { output->nested->value.set(10); }
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

struct optional_field_bundle_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSB<OptionalFieldBundle>> output;

    void start(EvaluationContext *context) const {
        output->first.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output->second.set(2); }
};

struct partially_valid_list_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSL<TS<i64>, 2>> output;

    void start(EvaluationContext *context) const {
        output[0].set(1);
        output[1].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output[1].invalidate();
        } else {
            output[1].set(12);
        }

        if ((*step)++ == 0) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

struct invalidating_list_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TSL<TS<i64>, 2>> output;

    void start(EvaluationContext *context) const {
        output[0].set(1);
        output[1].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[1].invalidate(); }
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

struct partially_valid_dictionary_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSD<i64, TS<i64>>> output;

    void start(EvaluationContext *context) const {
        output[1].set(10);
        output[2].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output[1].invalidate();
        } else if (*step == 1) {
            output.erase(1);
        } else {
            output.erase(2);
        }

        if (++*step != 3) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

} // namespace

TEST_CASE("all_valid checks every item in a fixed list",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][feature:collection-change]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = node<partially_valid_list_source>();
    const auto sum = node([](In<TSL<TS<i64>, 2>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? input.value() : i64{-1});
    });
    const auto graph = [&] { return observe(sum(source())); };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({11, -1, 13}));
}

TEST_CASE("all_valid rejects an unpopulated fixed-list position",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[feature:collection-change]") {
    struct partially_valid_fixed_list_source {
        Out<TSL<TS<i64>, 3>> output;

        void start() const {
            output[0].set(10);
            output[2].set(20);
        }
    };

    const auto source = node<partially_valid_fixed_list_source>();
    const auto query = node([](In<TSL<TS<i64>, 3>> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto gate = node([](In<TSL<TS<i64>, 3>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[2]);
    });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&] {
        const auto input = source();
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph).equals({0}));
}

TEST_CASE("an erased all_valid input uses the resolved runtime type",
          "[evaluation][validity][feature:validity][validity:all-valid][declaration:generic]"
          "[series:fixed-tsl][transition:invalidation][transition:revalidation]") {
    const auto source = node<partially_valid_list_source>();
    const auto query = node([](In<TS_T> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto gate = node([](In<TS_T, all_valid>, Out<TS<i64>> output) { output.set(1); });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&] {
        const auto input = source();
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph).equals({3, 0, 3}));
}

TEST_CASE("all_valid treats a valid empty set as valid",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tss][transition:start]") {
    struct empty_set_source {
        Out<TSS<i64>> output;

        void start() const { output.clear(); }
    };

    const auto source = node<empty_set_source>();
    const auto query = node([](In<TSS<i64>> input, Out<TS<bool>> output) { output.set(input.all_valid()); });
    const auto gate = node([](In<TSS<i64>, all_valid> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto report = node([](In<TS<bool>> query, In<TS<i64>, optional> gate, Out<TS<i64>> output) {
        output.set((query.value() ? 2 : 0) + (gate.valid() ? 1 : 0));
    });
    const auto graph = [&] {
        const auto input = source();
        return report(query(input), gate(input));
    };

    CHECK(testing::test_graph(graph).equals({3}));
}

TEST_CASE("all_valid reports an invalid root even when the collection has no data",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][transition:start]") {
    const auto source = node([](Out<TSL<TS<i64>, 2>>) {});
    const auto query = node(
        eval = [](In<TSL<TS<i64>, 2>, optional> input, Out<TS<bool>> output) { output.set(input.all_valid()); },
        schedule_on_start = true);
    const auto graph = [&] { return query(source()); };

    CHECK(testing::test_graph(graph).equals({false}));
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

    const auto source = node<partially_valid_list_source>();
    const auto sum = node<sum_valid_list>();
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? input.value() : i64{-1});
    });
    const auto graph = [&] { return observe(sum(source())); };

    CHECK(testing::test_graph(graph).equals({11, -1, 13}));
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
    const auto source = node<optional_field_bundle_source>();
    const auto sum = node([](In<TSB<OptionalFieldBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->first + *input->second);
    });
    const auto graph = [&] { return sum(source()); };

    const auto actual = testing::test_graph(graph);

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
    const auto source = node<changing_derived_bundle_source>();
    const auto inspect = node([](In<TSB<ValidityBaseBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->value);
    });
    const auto graph = [&] { return inspect(source()); };

    CHECK(testing::test_graph(graph).equals({1, std::nullopt, 2}));
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
    const auto source = node<nested_derived_bundle_source>();
    const auto inspect = node([](In<TSB<NestedValidityBaseBundle>, all_valid> input, Out<TS<bool>> output) {
        output.set(input.all_valid() && !input->nested.all_valid());
    });
    const auto graph = [&] { return inspect(source()); };

    CHECK(testing::test_graph(graph).equals({true}));
}

TEST_CASE("all_valid follows a selected reference using the declared base schema",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsb][series:reference]"
          "[schema:named][binding:reference-to-value][representation:covariant]"
          "[transition:invalidation][transition:revalidation]") {
    const auto first = node<validity_derived_bundle_source>();
    const auto second_value = node<invalidating_leaf_source>();
    const auto second = [&] {
        return bundle<TSB<ValidityDerivedBundle>>("value"_n = second_value(),
                                                  "extra"_n = operators::const_(i64{2}));
    };
    const auto select = node<validity_bundle_reference_selector>();
    const auto inspect = node([](In<TSB<ValidityBaseBundle>, all_valid> input, Out<TS<i64>> output) {
        output.set(*input->value);
    });
    const auto graph = [&](wp<TS<i64>> choose_second) {
        return inspect(select(choose_second, first(i64{1}).as<REF<TSB<ValidityDerivedBundle>>>(),
                              second().as<REF<TSB<ValidityDerivedBundle>>>()));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{0, 1, 1}).equals({1, std::nullopt, 3}));
}

TEST_CASE("all_valid ignores removed TSD items and accepts an empty live set",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][feature:collection-change]"
          "[transition:invalidation][transition:removal]") {
    const auto source = node<partially_valid_dictionary_source>();
    const auto sum = node([](In<TS<i64>, all_valid> constant, In<TS<i64>> value, i64 adjustment,
                             In<TSD<i64, TS<i64>>, all_valid> input, Out<TS<i64>> output) {
        auto total = i64{};
        input.for_each_value([&](const auto &item_value) { total += *item_value; });
        output.set(*constant + *value + adjustment + total);
    });
    const auto graph = [&](wp<TS<i64>> value) {
        return sum(operators::const_(i64{42}), value, i64{1}, source());
    };

    const auto actual = testing::test_graph(graph, std::array<i64, 4>{0, 0, 0, 0});

    CHECK(actual.equals({73, std::nullopt, 63, 43}));
}

TEST_CASE("all_valid descriptors stay aligned across collection inputs, constants, and scalars",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl][series:tsd]"
          "[guarantee:const][transition:start]") {
    struct valid_list_source {
        Out<TSL<TS<i64>, 2>> output;

        void start() const {
            output[0].set(10);
            output[1].set(20);
        }
    };
    struct valid_dictionary_source {
        Out<TSD<i64, TS<i64>>> output;

        void start() const { output[1].set(30); }
    };

    const auto list_source = node<valid_list_source>();
    const auto dictionary_source = node<valid_dictionary_source>();
    const auto sum = node([](In<TS<i64>, all_valid> constant, In<TSL<TS<i64>, 2>, all_valid> list,
                             In<TS<i64>> pulse, i64 adjustment, In<TSD<i64, TS<i64>>, all_valid> dictionary,
                             Out<TS<i64>> output) {
        output.set(*constant + *list[0] + *list[1] + *pulse + adjustment + *dictionary[1]);
    });
    const auto graph = [&](wp<TS<i64>> pulse) {
        return sum(operators::const_(i64{42}), list_source(), pulse, i64{2}, dictionary_source());
    };

    CHECK(testing::test_graph(graph, std::array<i64, 1>{1}).equals({105}));
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
    const auto make_dictionary = node(
        [](In<TS<i64>> input, Out<TSD<i64, TS<i64>>> output) { output[1].set(*input); });
    const auto inspect = node([](In<TSD<i64, TS<i64>>, all_valid> dictionary,
                                 In<REF<TS<i64>>, optional> missing, Out<TS<i64>> output) {
        output.set(static_cast<i64>(dictionary.size() * 10) + static_cast<i64>(missing.valid()));
    });
    const auto graph = [&](wp<TS<i64>> input) {
        const auto dictionary = make_dictionary(input);
        return inspect(dictionary, dictionary[2]);
    };

    CHECK(testing::test_graph(graph, std::array<i64, 2>{10, 20}).equals({10, 10}));
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

struct changing_dictionary_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSD<i64, TS<i64>>> output;

    void start(EvaluationContext *context) const {
        output[1].set(30);
        output[2].set(40);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output[2].invalidate();
        } else {
            output[2].set(42);
        }

        if ((*step)++ == 0) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

TEST_CASE("all_valid checks live items in sampled native dictionaries",
          "[evaluation][validity][feature:validity][validity:all-valid][series:tsd][series:reference]"
          "[feature:sampling][transition:invalidation][transition:revalidation]") {
    struct valid_dictionary_source {
        Out<TSD<i64, TS<i64>>> output;

        void start() const {
            output[1].set(10);
            output[2].set(20);
        }
    };

    const auto first = node<valid_dictionary_source>();
    const auto second = node<changing_dictionary_source>();
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
    const auto graph = [&](wp<TS<i64>> selection) {
        const auto selected = select(selection, first().as<REF<TSD<i64, TS<i64>>>>(),
                                     second().as<REF<TSD<i64, TS<i64>>>>());
        return inspect(selected);
    };

    CHECK(testing::test_graph(graph, std::array<i64, 4>{0, 0, 1, 1}).equals({10301, std::nullopt, 300, 10721}));
}

struct repeated_list_validity_changes_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSL<TS<i64>, 2>> output;

    void start(EvaluationContext *context) const {
        output[0].set(1);
        output[1].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        switch (*step) {
        case 0:
            output[1].invalidate();
            break;
        case 1:
            output[0].set(2);
            break;
        case 2:
            output[1].set(12);
            break;
        case 3:
            output[0].invalidate();
            break;
        case 4:
            output[1].set(13);
            break;
        default:
            return;
        }

        if (++*step < 5) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
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

    const auto source = node<repeated_list_validity_changes_source>();
    const auto observe = node<invalidation_counting_node>();
    const auto graph = [&] { return observe(source()); };

    CHECK(testing::test_graph(graph).equals({1000, 1, std::nullopt, 1001, 2}));
}

TEST_CASE("all_valid checks immediate nested items without recursively checking them",
          "[evaluation][validity][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[feature:collection-change]") {
    struct nested_list_source {
        Out<TSL<TSL<TS<i64>, 2>, 1>> output;

        void start() const { output[0][0].set(10); }
    };

    const auto source = node<nested_list_source>();
    const auto inspect = node([](In<TSL<TSL<TS<i64>, 2>, 1>, all_valid> input, Out<TS<bool>> output) {
        output.set(input[0].valid() && !input[0].all_valid());
    });
    const auto graph = [&] { return inspect(source()); };

    CHECK(testing::test_graph(graph).equals({true}));
}

TEST_CASE("all_valid on a reference checks the reference rather than its target items",
          "[evaluation][validity][feature:validity][validity:all-valid][series:reference]"
          "[series:fixed-tsl]") {
    struct partially_valid_list_source {
        Out<TSL<TS<i64>, 2>> output;

        void start() const { output[0].set(10); }
    };

    const auto source = node<partially_valid_list_source>();
    const auto inspect = node([](In<REF<TSL<TS<i64>, 2>>, all_valid> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&] { return inspect(source().as<REF<TSL<TS<i64>, 2>>>()); };

    CHECK(testing::test_graph(graph).equals({true}));
}

TEST_CASE("a passive all_valid input still invalidates its output",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:input-activity][series:fixed-tsl]"
          "[activity:passive][transition:invalidation]") {
    const auto source = node<invalidating_list_source>();
    const auto combine = node([](In<TSL<TS<i64>, 2>, passive, all_valid> input, In<TS<i64>> pulse,
                                 Out<TS<i64>> output) { output.set(*input[0] + *input[1] + *pulse); });
    const auto observe = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](wp<TS<i64>> pulse) { return observe(combine(source(), pulse)); };

    CHECK(testing::test_graph(graph, std::array<i64, 1>{100}).equals({true, false}));
}

TEST_CASE("dynamic activity does not suppress all_valid invalidation checks",
          "[evaluation][validity][feature:validity][validity:all-valid][feature:input-activity]"
          "[series:fixed-tsl][activity:active][transition:invalidation][transition:revalidation]") {
    const auto source = node<partially_valid_list_source>();
    const auto observe = node([](In<TSL<TS<i64>, 2>, all_valid, dynamic_activity> input, In<TS<i64>> pulse,
                                 Out<TS<i64>> output) {
        output.set(*input[0] + *input[1] + *pulse);
        input.make_passive();
    });
    const auto report = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });
    const auto graph = [&](wp<TS<i64>> pulse) { return report(observe(source(), pulse)); };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{1, 2, 3}).equals({true, false, true}));
}
