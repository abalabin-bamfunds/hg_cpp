#include <bg/graph.hpp>
#include <bg/graph/dynamic_node_subscriber_storage.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <array>
#include <chrono>
#include <new>
#include <optional>
#include <ranges>
#include <vector>

using namespace bg;

namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;

struct changing_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.set(20);
        } else if (*step == 1) {
            output.invalidate();
        } else {
            output.set(30);
            ++*step;
            return;
        }

        ++*step;
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct invalidating_then_revalidating_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.set(20);
        }
        ++*step;
    }
};

struct delayed_revalidating_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        } else {
            output.set(20);
        }
        ++*step;
    }
};

struct reference_pass {
    In<REF<TS<i64>>> input;
    Out<REF<TS<i64>>> output;

    void evaluate() const {
        output.copy(input);
        ++evaluation_count;
    }

    inline static int evaluation_count{};
};

struct optional_reference_consumer {
    In<TS<i64>, optional> input;

    void evaluate() const {}
};

struct sampled_left_source {
    Out<TS<i64>> output;
    void start() const { output.set(10); }
    void evaluate() const {}
};

struct sampled_right_source {
    Out<TS<i64>> output;
    void start() const { output.set(20); }
    void evaluate() const {}
};

template <template <typename, auto...> typename Field> struct DictionaryBundle {
    Field<TSD<i64, TS<i64>>> dictionary;
};

template <template <typename, auto...> typename Field> struct NativeReferenceBundle {
    Field<TS<i64>> value;
    Field<REF<TS<i64>>> selected;
};

template <template <typename, auto...> typename Field> struct AdaptedReferenceBundle {
    Field<REF<TS<i64>>> value;
    Field<TS<i64>> selected;
};

template <template <typename, auto...> typename Field> struct NestedReferenceBundle {
    Field<TSL<TS<i64>, 1>> values;
};

struct ReferenceObservation {
    bool valid;
    bool modified;
    bool became_valid;
    bool became_invalid;
    i64 value;
    EvaluationTime last_updated;

    bool operator==(const ReferenceObservation &) const = default;
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed target",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = node<changing_reference_target>();

    const auto pass = node<reference_pass>();
    reference_pass::evaluation_count = 0;
    const auto graph = [&] { return pass(source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20, std::nullopt, 30}));
}

TEST_CASE("a missing TSD root converts to and from item references before becoming valid",
          "[evaluation][reference][tsd][validity][series:tsd][series:value][transition:invalidation]"
          "[transition:revalidation][binding:value-to-reference][binding:reference-to-value]"
          "[feature:reference]") {
    struct select_dictionary {
        In<TS<bool>> select_live;
        In<REF<TSD<i64, REF<TS<i64>>>>, optional> live;
        In<REF<TSD<i64, REF<TS<i64>>>>, optional> missing;
        Out<REF<TSD<i64, REF<TS<i64>>>>> output;

        void evaluate() const { output.copy(*select_live ? live : missing); }
    };

    const auto observe_value = node([](In<TSD<i64, TS<i64>>> dictionary, Out<TS<i64>> output) {
        output.set(dictionary.contains(1) ? *dictionary[1] : -1);
    });
    const auto select = node<select_dictionary>();
    const auto graph = [&](wp<TS<bool>> select_live, wp<TSD<i64, TS<i64>>> live) {
        const auto missing = operators::nothing[type<TSD<i64, TS<i64>>>]();
        return observe_value(select(select_live, live, missing));
    };

    CHECK(bg::run_graph(graph, bg::input<TS<bool>>({false, true, false}),
                        bg::input<TSD<i64, TS<i64>>>({{{1, 10}}, literal::SKIP, literal::SKIP}))
              .equals<i64>({std::nullopt, 10, std::nullopt}));
}

TEST_CASE("a generated reference value stays stable while its target changes",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[transition:invalidation][transition:revalidation][binding:value-to-reference]"
          "[binding:reference-to-value][guarantee:ordinary][representation:native]") {
    const auto source = node<changing_reference_target>();
    auto reference_evaluations = 0;
    auto target_evaluations = 0;
    const auto forward = node([&](In<TSL<REF<TS<i64>>, 1>> input, Out<REF<TS<i64>>> output) {
        ++reference_evaluations;
        output.copy(input[0]);
    });
    const auto read = node([&](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        ++target_evaluations;
        if (input.valid()) {
            output.set(*input);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&] {
        return read(forward(tsl(source())));
    };

    CHECK(bg::run_graph(graph).equals({10, 20, std::nullopt, 30}));
    CHECK(reference_evaluations == 1);
    CHECK(target_evaluations == 4);
}

TEST_CASE("an ordinary reference can retarget a value input between stronger targets",
          "[feature:reference][feature:type-system][feature:test-graph][series:value][scalar:floating-point]"
          "[guarantee:ordinary][guarantee:final][guarantee:const][binding:reference-to-value]"
          "[shape:chain][transition:update][definition:function]") {
    const auto final_value = node([](Out<TS<double, TimeSeriesGuarantee::final>> output) {
        output.set(1.0);
    });

    const auto select = node(
        [](In<TS<bool>> use_constant,
           In<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> first,
           In<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> second,
           Out<REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>> output) {
            output.copy(*use_constant ? second : first);
        });

    const auto copy = node(
        [](In<TS<double, guarantee_variable<"G">>> input,
           Out<TS<double, guarantee_variable<"G">>> output) {
            output.set(*input);
        });

    const auto graph = [&](wp<TS<bool>> use_constant) {
        const auto constant_value = operators::const_(2.0);
        const auto selected = select(use_constant, final_value(), constant_value);
        const auto output = copy(selected);

        CHECK(&output.type() == &TS<double>::type());
        return output;
    };

    const auto actual = bg::run_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals<double>({1.0, 2.0, 1.0, 2.0}));
}

TEST_CASE("a deduplicated reference retarget samples equal constant values",
          "[feature:reference][feature:type-system][feature:test-graph][series:value][scalar:floating-point]"
          "[guarantee:deduplicated][guarantee:const][binding:reference-to-value]"
          "[shape:chain][transition:update][definition:function]") {
    const auto select = node(
        [](In<TS<bool>> use_second,
           In<REF<TS<double, TimeSeriesGuarantee::const_>,
                          TimeSeriesGuarantee::deduplicated>> first,
           In<REF<TS<double, TimeSeriesGuarantee::const_>,
                          TimeSeriesGuarantee::deduplicated>> second,
           Out<REF<TS<double, TimeSeriesGuarantee::const_>,
                           TimeSeriesGuarantee::deduplicated>> output) {
            output.copy(*use_second ? second : first);
        });

    const auto first = node(
        [](Out<TS<double, TimeSeriesGuarantee::const_>> output) { output.set(4.0); });

    const auto second = node(
        [](Out<TS<double, TimeSeriesGuarantee::const_>> output) { output.set(4.0); });

    const auto read = node([](In<TS<double, guarantee_variable<"G">>> input) { return *input; });

    const auto graph = [&](wp<TS<bool>> use_second) {
        return read(select(use_second, first(), second()));
    };

    const auto actual = bg::run_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals<double>({4.0, 4.0, 4.0, 4.0}));
}

TEST_CASE("a dereferenced list keeps the target runtime type when only guarantees differ",
          "[evaluation][reference][tsl][feature:reference][series:fixed-tsl][guarantee:ordinary]"
          "[guarantee:final][binding:reference-to-value]") {
    const auto first = node(
        [](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(1); });

    const auto second = node(
        [](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(2); });

    const auto pass = node(
        [](In<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>>> input,
           Out<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>>> output) {
            output.copy(input);
        });
    auto target_type = static_cast<const TimeSeriesType *>(nullptr);
    auto received_type = static_cast<const TimeSeriesType *>(nullptr);

    const auto read = node([&](In<TSL<TS<i64>, 2>> input) {
        received_type = &input.runtime_type().type();
        return *input[0] + *input[1];
    });

    const auto graph = [&] {
        const auto values = tsl(first(), second());
        target_type = &values.type();
        return read(pass(values));
    };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<i64>({3}));
    CHECK(received_type == target_type);
}

TEST_CASE("a materialized free TSL samples retargeted ordinary references",
          "[evaluation][reference][tsl][feature:reference][feature:test-graph][series:fixed-tsl]"
          "[guarantee:ordinary][guarantee:const][binding:reference-to-value][transition:update]") {
    const auto read = node([](In<TSL<TS<i64, G>, 1>> input) { return *input[0]; });

    const auto select = node(
        [](In<TS<bool>> use_second,
           In<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::ordinary>,
                  passive> first,
           In<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::ordinary>,
                  passive> second,
           Out<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::ordinary>>
               output) { output.copy(*use_second ? second : first); });

    const auto graph = [&](wp<TS<bool>> use_second) {
        const auto first = operators::const_(i64{4});
        const auto second = operators::const_(i64{7});
        const auto values = tsl(select(use_second, first, second));
        const auto &type = dynamic_cast<const TimeSeriesListType &>(values.type());
        CHECK(type.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return read(values);
    };

    const auto actual = bg::run_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals({4, 7, 4, 7}));
}

TEST_CASE("an ordinary list reference samples the selected list with final items",
          "[evaluation][reference][feature:reference][series:fixed-tsl][guarantee:ordinary]"
          "[guarantee:final][binding:reference-to-value][activity:active][transition:update][shape:chain]") {
    const auto first_item = node(
        [](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(10); });

    const auto first_other_item = node(
        [](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(11); });

    const auto second_item = node(
        [](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(20); });

    const auto second_other_item = node(
        [](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(21); });

    const auto select = node(
        [](In<TS<bool>> use_second,
           In<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>,
                          TimeSeriesGuarantee::ordinary>> first,
           In<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>,
                          TimeSeriesGuarantee::ordinary>> second,
           Out<REF<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>,
                           TimeSeriesGuarantee::ordinary>> output) {
            output.copy(*use_second ? second : first);
        });

    const auto read = node([](In<TSL<TS<i64>, 2>> input) { return *input[0]; });

    const auto graph = [&](wp<TS<bool>> use_second) {
        const auto first = tsl(first_item(), first_other_item());
        const auto second = tsl(second_item(), second_other_item());
        return read(select(use_second, first, second));
    };

    const auto actual = bg::run_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals<i64>({10, 20, 10, 20}));
}

TEST_CASE("plain and notifying inputs share one sampled dictionary",
          "[evaluation][reference][series:tsd][series:tsb][feature:sampling][transition:update]"
          "[binding:reference-to-value]") {
    const auto select = node(
        [](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
           In<REF<TSD<i64, TS<i64>>>> second,
           Out<REF<TSD<i64, TS<i64>>>> output) {
            output.copy(*use_second ? second : first);
        });

    const auto store_reference = node(
        [](In<REF<TSD<i64, TS<i64>>>> input,
           Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
            output[0].copy(input);
        });

    const auto compare = node(
        [](In<TSD<i64, TS<i64>>> direct,
           In<TSB<DictionaryBundle>> packaged, Out<TS<bool>> output) {
            output.set(direct.data() == packaged->dictionary.data());
        });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second,
                           wp<TS<bool>> use_second) {
        const auto selected = store_reference(select(use_second, first, second));
        const auto item = selected[i64{0}];
        return compare(item, bundle<DictionaryBundle>(item));
    };

    const auto actual = bg::run_graph(graph, {{{1, 10}}, {{1, 10}}}, {{{2, 20}}, {{2, 20}}},
                                                {false, true});

    CHECK(actual.equals({true, true}));
}

TEST_CASE("a nested graph releases its sampled reference change when it stops",
          "[evaluation][nested][reference][series:tsd][feature:sampling][feature:nested-graph]"
          "[transition:update][transition:stop][transition:restart][binding:reference-to-value]") {
    const auto select = node(
        [](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
           In<REF<TSD<i64, TS<i64>>>> second,
           Out<REF<TSD<i64, TS<i64>>>> output) {
            output.copy(*use_second ? second : first);
        });

    const auto inspect = node(
        [](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
            auto added = 0;
            auto removed = 0;
            input.for_each_added([&](const auto &key) { added |= 1 << *key; });
            input.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
            output.set(added * 100 + removed * 10);
        });

    const auto combine = node(
        [](In<TS<i64>, optional> inner, In<TS<i64>> direct,
           Out<TS<i64>> output) {
            output.set((inner.valid() ? *inner : 0) + *direct);
        });

    const auto inner = graph(
        "name"_n = "inspect_sampled_dictionary",
        [&](wp<TSD<i64, TS<i64>>> input) -> wp<TS<i64>> {
            return inspect(input);
        });

    const auto outer = [&](wp<TSD<i64, TS<i64>>> first,
                           wp<TSD<i64, TS<i64>>> second,
                           wp<TS<bool>> use_second, wp<TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        return combine(nested(inner, run, selected), inspect(selected));
    };

    const auto actual = bg::run_graph(
        outer, {{{1, 10}}, literal::SKIP, literal::SKIP, literal::SKIP},
        {{{2, 20}}, literal::SKIP, literal::SKIP, literal::SKIP},
        {false, literal::SKIP, true, false}, {true, true, false, true});

    CHECK(actual.equals({400, std::nullopt, 420, 440}));
}

TEST_CASE("a default reference output does not schedule subscribers for the same reference",
          "[evaluation][reference][feature:reference][transition:update][series:value][scalar:integer]"
          "[guarantee:deduplicated][binding:value-to-reference][binding:reference-to-reference]") {
    const auto forward = node([](In<TS<i64>>, In<REF<TS<i64>>, passive> input,
                                     Out<REF<TS<i64>>> output) { output.copy(input); });
    auto evaluations = 0;

    const auto sink = node([&](In<REF<TS<i64>>>) { ++evaluations; });

    const auto graph = [&](wp<TS<i64>> input) {
        sink(forward(input, input));
        return input;
    };

    const auto actual = bg::run_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
    CHECK(evaluations == 1);
}

TEST_CASE("an ordinary reference republishes ticks without changing its dereferenced target",
          "[evaluation][reference][validity][series:value][transition:first-tick][transition:update]"
          "[guarantee:ordinary][binding:reference-to-value][feature:reference]") {
    struct republish_reference {
        In<TS<i64>> trigger;
        In<REF<TS<i64>>, passive> source;
        Out<REF<TS<i64>, TimeSeriesGuarantee::ordinary>> output;

        void start() const { output.copy(source); }
        void evaluate() const { output.copy(source); }
    };

    auto reference_ticks = 0;
    const auto source = node<sampled_left_source>();

    const auto republish = node<republish_reference>();

    const auto observe_reference = node([&](In<REF<TS<i64>>>) { ++reference_ticks; });

    const auto observe_value = node([](In<TS<i64>, optional> input, In<TS<i64>, optional> trigger) {
        (void)trigger;
        return ReferenceObservation{input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                    input.valid() ? *input : i64{}, input.last_updated()};
    });

    const auto graph = [&](wp<TS<i64>> republish_trigger, wp<TS<i64>> observation_trigger) {
        const auto reference = republish(republish_trigger, source());
        observe_reference(reference);
        return observe_value(reference, observation_trigger);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto actual =
        bg::run_graph(graph, GraphRunOptions{.start_time = start_time}, ticks({literal::SKIP, i64{1}, literal::SKIP}),
                      ticks({literal::SKIP, literal::SKIP, i64{1}}));

    const auto expected = std::initializer_list<std::optional<ReferenceObservation>>{
        ReferenceObservation{true, true, true, false, 10, start_time},
        std::nullopt,
        ReferenceObservation{true, false, false, false, 10, start_time},
    };
    CHECK(actual.equals<ReferenceObservation>(expected));
    CHECK(reference_ticks == 2);
}

namespace {

struct deferred_state_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        auto value = i64{40};
        if (*step == 0) {
            value = 20;
        } else if (*step == 1) {
            value = 30;
        }
        output.set(value);
        if (*step == 0) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{3});
        } else if (*step == 1) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        }
        ++*step;
    }
};

struct deferred_state_reference_republisher {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TS<i64>>, passive> source;
    State<int> step;
    Out<REF<TS<i64>, TimeSeriesGuarantee::ordinary>> output;

    void start(EvaluationContext *context) const {
        output.copy(source);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(EvaluationContext *context) const {
        output.copy(source);
        if (*step == 0) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else if (*step == 1) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
        }
        ++*step;
    }
};

struct deferred_state_trigger {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{3});
    }

    void evaluate() const { output.set(1); }
};

struct deferred_rebind_target {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(20); }
};

struct deferred_reference_rebinder {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TS<i64>>, passive> source;
    In<REF<TS<i64>>, passive> replacement;
    State<int> step;
    Out<REF<TS<i64>, TimeSeriesGuarantee::ordinary>> output;

    void start(EvaluationContext *context) const {
        output.copy(source);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.copy(source);
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.copy(replacement);
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("a same-target reference tick retains the target state until input resolution sees a target update",
          "[evaluation][reference][validity][series:value][transition:update][feature:reference]"
          "[binding:reference-to-value][lifecycle]") {
    auto observations = std::vector<ReferenceObservation>{};
    auto passive_observations = std::vector<ReferenceObservation>{};
    const auto source = node<deferred_state_reference_target>();

    const auto republish = node<deferred_state_reference_republisher>();

    const auto trigger = node<deferred_state_trigger>();

    const auto observe = node([&](In<TS<i64>, optional> input) {
        observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                input.valid() ? *input : i64{}, input.last_updated()});
    });
    const auto observe_passive = node([&](In<TS<i64>, passive, optional> input, In<TS<i64>>) {
        passive_observations.push_back({input.valid(), input.modified(), input.became_valid(),
                                        input.became_invalid(), input.valid() ? *input : i64{},
                                        input.last_updated()});
    });
    auto wiring = GraphWiring{};
    auto first_observer_identity = NodeCallIdentity{};
    auto passive_observer_identity = NodeCallIdentity{};
    {
        const auto context = WiringContext{wiring};
        const auto target = source();
        const auto reference = republish(target);
        const auto active_arguments = std::array{WiringArgument{reference}};
        first_observer_identity = wiring.invoke_node(observe, active_arguments).identity;
        const auto passive_arguments = std::array{WiringArgument{reference}, WiringArgument{trigger()}};
        passive_observer_identity = wiring.invoke_node(observe_passive, passive_arguments).identity;
    }
    wiring.finalize();
    const auto first_observer_index = wiring.node_index(first_observer_identity);
    const auto passive_observer_index = wiring.node_index(passive_observer_identity);
    const auto compiled = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto global_state = GlobalState{};
    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto target_update_time = start_time + std::chrono::microseconds{1};
    const auto reference_update_time = start_time + std::chrono::microseconds{2};
    const auto next_target_update_time = start_time + std::chrono::microseconds{4};

    for (const auto allowed_allocations : {std::size_t{}, std::size_t{1}}) {
        observations.clear();
        passive_observations.clear();
        {
            auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
            runtime.start(start_time);
            runtime.evaluate(start_time);
            REQUIRE(observations.size() == 1);
            CHECK(observations.front() == ReferenceObservation{true, true, true, false, 10, start_time});
            runtime.evaluate(target_update_time);
            REQUIRE(observations.size() == 2);
            CHECK(observations.back() == ReferenceObservation{true, true, false, false, 20, target_update_time});

            memory.fail_after(allowed_allocations);
            CHECK_THROWS_AS(runtime.evaluate(reference_update_time), std::bad_alloc);
            memory.allow_allocations();
            CHECK(runtime.node(first_observer_index).reference_input(0)->has_deferred_state() ==
                  (allowed_allocations == 1));
            CHECK_FALSE(runtime.node(passive_observer_index).reference_input(0)->has_deferred_state());
        }
        CHECK(memory.allocations() == 0);
    }

    observations.clear();
    passive_observations.clear();
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        runtime.start(start_time);
        runtime.evaluate(start_time);
        REQUIRE(observations.size() == 1);
        CHECK(observations.front() == ReferenceObservation{true, true, true, false, 10, start_time});
        runtime.evaluate(target_update_time);
        REQUIRE(observations.size() == 2);
        CHECK(observations.back() == ReferenceObservation{true, true, false, false, 20, target_update_time});

        const auto bytes_before_reference_update = memory.allocated_bytes();

        runtime.evaluate(reference_update_time);
        CHECK(observations.size() == 2);
        CHECK(runtime.node(first_observer_index)
                  .time_series_input(0, reference_update_time)
                  .last_updated() == target_update_time);
        const auto bytes_after_reference_update = memory.allocated_bytes();
        const auto deferred_count = static_cast<std::size_t>(
                                        runtime.node(first_observer_index).reference_input(0)->has_deferred_state()) +
                                    static_cast<std::size_t>(
                                        runtime.node(passive_observer_index).reference_input(0)->has_deferred_state());
        REQUIRE(deferred_count == 2);
        CHECK(bytes_after_reference_update ==
              bytes_before_reference_update + deferred_count * sizeof(DeferredReferenceInputState));
        CHECK(details::ReferenceProvider::get(
                  runtime.node(first_observer_index).reference_input(0)->reference()) == nullptr);

        const auto retained_reference_update_time = reference_update_time + std::chrono::microseconds{1};
        runtime.evaluate(retained_reference_update_time);
        CHECK(memory.allocated_bytes() == bytes_after_reference_update);
        REQUIRE(passive_observations.size() == 2);
        CHECK(passive_observations.back() ==
              ReferenceObservation{true, false, false, false, 20, target_update_time});

        runtime.evaluate(next_target_update_time);
        REQUIRE(observations.size() == 3);
        CHECK(observations.back() == ReferenceObservation{true, true, false, false, 30, next_target_update_time});
        CHECK_FALSE(runtime.node(first_observer_index).reference_input(0)->has_deferred_state());
        CHECK(runtime.node(passive_observer_index).reference_input(0)->has_deferred_state());
        CHECK(memory.allocated_bytes() ==
              bytes_after_reference_update - sizeof(DeferredReferenceInputState));

        const auto passive_input =
            runtime.node(passive_observer_index).time_series_input(0, next_target_update_time);
        CHECK(passive_input.last_updated() == next_target_update_time);
        CHECK(passive_input.modified());
        CHECK_FALSE(runtime.node(passive_observer_index).reference_input(0)->has_deferred_state());
        CHECK(memory.allocated_bytes() ==
              bytes_after_reference_update - deferred_count * sizeof(DeferredReferenceInputState));
    }

    CHECK(memory.allocations() == 0);

    observations.clear();
    passive_observations.clear();
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        runtime.start(start_time);
        runtime.evaluate(start_time);
        runtime.evaluate(target_update_time);
        runtime.evaluate(reference_update_time);

        auto *passive_input = runtime.node(passive_observer_index).reference_input(0);
        REQUIRE(passive_input->has_deferred_state());

        runtime.evaluate(start_time + std::chrono::microseconds{3});
        runtime.evaluate(next_target_update_time);
        CHECK(passive_input->has_deferred_state());

        const auto newer_reference_update_time = next_target_update_time + std::chrono::microseconds{1};
        runtime.evaluate(newer_reference_update_time);
        const auto before_target_catches_up =
            runtime.node(passive_observer_index).time_series_input(0, newer_reference_update_time);
        CHECK(before_target_catches_up.last_updated() == next_target_update_time);
        CHECK(passive_input->has_deferred_state());

        const auto repeated_read =
            runtime.node(passive_observer_index).time_series_input(0, newer_reference_update_time);
        CHECK(repeated_read.last_updated() == next_target_update_time);
        CHECK(passive_input->has_deferred_state());

        const auto final_target_update_time = newer_reference_update_time + std::chrono::microseconds{1};
        runtime.evaluate(final_target_update_time);
        CHECK(passive_input->has_deferred_state());
        const auto caught_up = runtime.node(passive_observer_index).time_series_input(0, final_target_update_time);
        CHECK(caught_up.last_updated() == final_target_update_time);
        CHECK_FALSE(passive_input->has_deferred_state());
    }
    CHECK(memory.allocations() == 0);

    passive_observations.clear();
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        runtime.start(start_time);
        runtime.evaluate(start_time);
        runtime.evaluate(target_update_time);
        runtime.evaluate(reference_update_time);
        auto *input = runtime.node(first_observer_index).reference_input(0);
        REQUIRE(input->has_deferred_state());
    }
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a failed reference rebind keeps its deferred input and releases the new subscription",
          "[evaluation][reference][validity][feature:reference][lifecycle][allocation-failure]") {
    auto sink_evaluations = std::size_t{};
    auto sink_observations = std::vector<ReferenceObservation>{};
    const auto source = node<deferred_rebind_target>();

    const auto replacement = node<sampled_right_source>();

    const auto pass = node<reference_pass>();

    const auto rebind = node<deferred_reference_rebinder>();

    const auto replacement_sink = node<optional_reference_consumer>();

    const auto sink = node([&](In<TS<i64>, optional> input) {
        ++sink_evaluations;
        sink_observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                    input.valid() ? *input : i64{}, input.last_updated()});
    });
    auto wiring = GraphWiring{};
    auto replacement_sink_identity = NodeCallIdentity{};
    auto sink_identity = NodeCallIdentity{};
    {
        const auto context = WiringContext{wiring};
        const auto source_reference = source();
        const auto replacement_reference = pass(replacement());
        const auto replacement_sink_arguments = std::array{WiringArgument{replacement_reference}};
        replacement_sink_identity = wiring.invoke_node(replacement_sink, replacement_sink_arguments).identity;
        const auto rebind_output = rebind(source_reference, replacement_reference);
        const auto sink_arguments = std::array{WiringArgument{rebind_output}};
        sink_identity = wiring.invoke_node(sink, sink_arguments).identity;
    }

    wiring.finalize();
    const auto replacement_sink_index = wiring.node_index(replacement_sink_identity);
    const auto sink_node_index = wiring.node_index(sink_identity);
    const auto compiled = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto global_state = GlobalState{};
    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto target_update_time = start_time + std::chrono::microseconds{1};
    const auto reference_tick_time = start_time + std::chrono::microseconds{2};
    const auto rebind_time = start_time + std::chrono::microseconds{3};

    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        runtime.start(start_time);
        runtime.evaluate(start_time);
        REQUIRE(sink_observations.size() == 1);
        CHECK(sink_observations.back() == ReferenceObservation{true, true, true, false, 10, start_time});
        runtime.evaluate(target_update_time);
        REQUIRE(sink_observations.size() == 2);
        CHECK(sink_observations.back() == ReferenceObservation{true, true, false, false, 20, target_update_time});
        runtime.evaluate(reference_tick_time);
        auto *input = runtime.node(sink_node_index).reference_input(0);
        REQUIRE(input->has_deferred_state());
        auto *deferred_state = input->deferred_state();
        const auto original_reference = deferred_state->reference;
        auto *replacement_input = runtime.node(replacement_sink_index).reference_input(0);
        REQUIRE(replacement_input != nullptr);
        auto *replacement_root = replacement_input->reference().get<details::RootReference>();
        REQUIRE(replacement_root != nullptr);
        REQUIRE(replacement_root->has_inline_subscriber());

        memory.fail_after(0);
        CHECK_THROWS_AS(runtime.evaluate(rebind_time), std::bad_alloc);
        memory.allow_allocations();

        CHECK(input->has_deferred_state());
        CHECK(input->deferred_state() == deferred_state);
        CHECK(input->deferred_state()->reference == original_reference);
        CHECK(runtime.node(sink_node_index).time_series_input(0, rebind_time).last_updated() == target_update_time);
        CHECK(replacement_root->has_inline_subscriber());
        CHECK_FALSE(replacement_root->has_dynamic_subscribers());
        CHECK(sink_evaluations == 2);
    }

    CHECK(memory.allocations() == 0);

    sink_evaluations = 0;
    sink_observations.clear();
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        runtime.start(start_time);
        runtime.evaluate(start_time);
        REQUIRE(sink_observations.size() == 1);
        CHECK(sink_observations.back() == ReferenceObservation{true, true, true, false, 10, start_time});
        runtime.evaluate(target_update_time);
        REQUIRE(sink_observations.size() == 2);
        CHECK(sink_observations.back() == ReferenceObservation{true, true, false, false, 20, target_update_time});
        runtime.evaluate(reference_tick_time);

        auto *input = runtime.node(sink_node_index).reference_input(0);
        REQUIRE(input->has_deferred_state());
        auto *replacement_input = runtime.node(replacement_sink_index).reference_input(0);
        REQUIRE(replacement_input != nullptr);
        const auto replacement_reference = replacement_input->reference();
        auto *replacement_root = replacement_reference.get<details::RootReference>();
        REQUIRE(replacement_root != nullptr);

        runtime.evaluate(rebind_time);

        CHECK_FALSE(input->has_deferred_state());
        CHECK(input->reference() == replacement_reference);
        const auto selected = runtime.node(sink_node_index).time_series_input(0, rebind_time);
        CHECK(selected.valid());
        CHECK(selected.as<TS<i64>>().value() == 20);
        CHECK(sink_evaluations == 3);
        CHECK(sink_observations.back().last_updated == rebind_time);
        CHECK(replacement_root->has_dynamic_subscribers());
    }

    CHECK(memory.allocations() == 0);
}

TEST_CASE("reference provider access preserves lazy allocation",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    struct optional_reference_consumer {
        In<TS<i64>, optional> input;

        void evaluate() const {}
    };

    const auto source = node<changing_reference_target>();

    const auto pass = node<reference_pass>();

    const auto sink = node<optional_reference_consumer>();
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        sink(pass(source()));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    runtime.start(MIN_DT + std::chrono::microseconds{1});
    runtime.evaluate(MIN_DT + std::chrono::microseconds{2});

    auto &root = *static_cast<details::RootReference *>(runtime.node(1).state());
    CHECK(root.has_inline_subscriber());
    CHECK_FALSE(root.has_provider());

    const auto reference = Reference{&root};
    CHECK(details::ReferenceProvider::get({}) == nullptr);
    CHECK(details::ReferenceProvider::get(reference) == nullptr);

    auto &provider = details::ReferenceProvider::require(reference);
    CHECK(details::ReferenceProvider::get(reference) == &provider);
    CHECK(root.has_provider());

    runtime.stop();
}

TEST_CASE("a dynamic input callback crosses a graph boundary immediately",
          "[evaluation][reference][feature:reference][shape:multiple][transition:update]") {
    auto wiring = GraphWiring{};
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto source_global_state = GlobalState{};
    auto source = RuntimeGraph{compiled, source_global_state};
    auto receiver_global_state = GlobalState{};
    auto receiver = RuntimeGraph{compiled, receiver_global_state};
    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto update_time = start_time + std::chrono::microseconds{1};
    source.start(start_time);
    receiver.start(start_time);
    source.evaluate(update_time);

    struct CallbackResult {
        RuntimeGraph *graph{};
        EvaluationTime time{MIN_DT};
        OutputChange change{};
        int calls{};
    } result;

    auto *subscribers = static_cast<details::DynamicSubscribers *>(nullptr);
    auto &subscription = details::DynamicSubscribers::ensure(subscribers, source.allocation());
    subscription.add(
        receiver, 0, reinterpret_cast<std::size_t>(&result), 0, false, false,
        [](OutputChangeContext &context, std::size_t, std::size_t data, OutputChange change) {
            auto &result = *reinterpret_cast<CallbackResult *>(data);
            result.graph = &context.runtime_graph();
            result.time = context.evaluation_time();
            result.change = change;
            ++result.calls;
        });

    source.notify_dynamic_subscribers(subscription, OutputChange::update);

    CHECK(result.graph == &receiver);
    CHECK(result.time == update_time);
    CHECK(result.change == OutputChange::update);
    CHECK(result.calls == 1);
    details::DynamicSubscribers::release(subscribers);
}

TEST_CASE("a second root subscriber promotes inline storage",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][shape:fan-out]") {
    auto wiring = GraphWiring{};
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto runtime_global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, runtime_global_state};
    auto root = details::RootReference{};
    root.owner_graph = &runtime;
    const auto reference = Reference{&root};

    details::ReferenceProvider::add_subscriber(reference, runtime, 0, 0, 0, true, false);

    CHECK(root.has_inline_subscriber());

    details::ReferenceProvider::remove_subscriber(reference, runtime, 0);

    CHECK_FALSE(root.has_inline_subscriber());
    CHECK_FALSE(root.has_dynamic_subscribers());

    details::ReferenceProvider::add_subscriber(reference, runtime, 0, 0, 0, true, false);
    details::ReferenceProvider::add_subscriber(reference, runtime, 8, 0, 1, true, false);

    CHECK_FALSE(root.has_inline_subscriber());
    CHECK(root.has_dynamic_subscribers());
    auto subscriber_count = std::size_t{};
    root.dynamic_subscribers()->for_each_node_subscriber([&](const auto &) { ++subscriber_count; });
    CHECK(subscriber_count == 2);

    details::ReferenceProvider::finalize_root(root, runtime.allocation());
}

TEST_CASE("dynamic node subscriber storage promotes transactionally",
          "[evaluation][feature:reference][allocation][shape:multiple]") {
    auto wiring = GraphWiring{};
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto graph = RuntimeGraph{compiled, global_state};
    auto other_global_state = GlobalState{};
    auto other_graph = RuntimeGraph{compiled, other_global_state};
    auto memory = counting_memory_resource{};
    auto subscribers = details::DynamicNodeSubscriberStorage<>{};

    STATIC_CHECK(sizeof(details::DynamicNodeSubscriberStorage<>) == 2 * sizeof(void *));
    subscribers.add(details::DynamicNodeSubscriber{graph, 0, 3, true, false, true}, allocator{memory});
    CHECK(subscribers.has_inline_subscriber());
    CHECK(memory.allocations() == 0);

    subscribers.add(details::DynamicNodeSubscriber{graph, 0, 7, false, true, false}, allocator{memory});
    CHECK(subscribers.has_inline_subscriber());
    CHECK(subscribers.inline_subscriber().node_index() == 3);
    CHECK(subscribers.inline_subscriber().active());
    CHECK(subscribers.inline_subscriber().controlled_activity());
    subscribers.remove(other_graph, 0);
    CHECK(subscribers.has_inline_subscriber());

    subscribers.remove(graph, 0);
    CHECK(subscribers.empty());
    CHECK_FALSE(subscribers.destination);
    subscribers.add(details::DynamicNodeSubscriber{graph, 0, 3, true, false, true}, allocator{memory});

    auto promoted = false;
    for (auto failure_point = std::size_t{}; failure_point < 8; ++failure_point) {
        memory.fail_after(failure_point);
        try {
            subscribers.add(details::DynamicNodeSubscriber{other_graph, 0, 5, true, false, false},
                            allocator{memory});
            promoted = true;
            memory.allow_allocations();
            break;
        } catch (const std::bad_alloc &) {
            memory.allow_allocations();
            CHECK(subscribers.has_inline_subscriber());
            CHECK(subscribers.inline_subscriber().graph == &graph);
            CHECK(subscribers.inline_subscriber().input_offset == 0);
            CHECK(subscribers.inline_subscriber().node_index() == 3);
            CHECK(memory.allocations() == 0);
        }
    }
    CHECK(promoted);
    CHECK(subscribers.has_dynamic_subscribers());

    auto subscriber_count = std::size_t{};
    subscribers.dynamic_subscribers()->for_each_node_subscriber([&](const auto &subscriber) {
        ++subscriber_count;
        if (subscriber.graph == &graph) {
            CHECK(subscriber.node_index() == 3);
            CHECK(subscriber.active());
            CHECK(subscriber.controlled_activity());
        }
    });
    CHECK(subscriber_count == 2);

    subscribers.remove(graph, 0);
    CHECK(subscribers.has_dynamic_subscribers());
    CHECK_FALSE(subscribers.empty());
    subscribers.remove(other_graph, 0);
    CHECK(subscribers.has_dynamic_subscribers());
    CHECK(subscribers.empty());
    subscribers.release();
    CHECK_FALSE(subscribers.destination);
    CHECK(memory.allocations() == 0);
}

namespace {

struct sampled_condition_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct invalidating_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct revalidating_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(20); }
};

struct reference_selector {
    In<TS<i64>> condition;
    In<REF<TS<i64>>, passive, optional> left;
    In<REF<TS<i64>>, passive, optional> right;
    Out<REF<TS<i64>>> output;

    void evaluate() const { *condition ? output.copy(right) : output.copy(left); }
};

struct dynamic_reference_value {
    In<TS<i64>, optional, dynamic_activity> input;
    In<TS<i64>> trigger;
    Out<TS<i64>> output;

    void evaluate() const {
        if (*trigger == 0) {
            input.make_passive();
        } else {
            input.make_active();
        }
        output.set(*input);
    }
};

struct dynamic_reference_unbinder {
    In<REF<TSL<TS<i64>, 1>>> collection;
    In<REF<TS<i64>>, dynamic_binding, optional> input;
    In<TS<i64>> trigger;
    Out<TS<bool>> output;

    void evaluate() const {
        if (*trigger == 0) {
            input.bind(collection[0]);
        }
        input.unbind();
        output.set(input.became_invalid());
    }
};

} // namespace

TEST_CASE("changing a reference samples its target",
          "[evaluation][reference][sampling][feature:reference][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][series:value]") {
    struct Observation {
        i64 value;
        EvaluationTime sample_time;
        EvaluationTime evaluation_time;

        bool operator==(const Observation &) const = default;
    };

    struct observe_sample {
        In<TS<i64>> input;
        Out<TS<Observation>> output;

        void evaluate(EvaluationContext *context) const {
            output.set(Observation{*input, input.last_updated(), context->evaluation_time()});
        }
    };

    const auto left = node<sampled_left_source>();

    const auto right = node<sampled_right_source>();

    const auto condition = node<sampled_condition_source>();

    const auto select = node<reference_selector>();

    const auto observe = node<observe_sample>();
    const auto graph = [&] { return observe(select(condition(), left(), right())); };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};

    const auto actual = bg::run_graph(graph, GraphRunOptions{.start_time = start_time});

    CHECK(actual.equals<Observation>({
        Observation{10, start_time, start_time},
        Observation{20, start_time + std::chrono::microseconds{1},
                    start_time + std::chrono::microseconds{1}},
    }));
}

TEST_CASE("a dereferenced reference reports the transition of the selected value",
          "[evaluation][reference][sampling][validity][feature:reference][series:value]"
          "[binding:reference-to-value][transition:update][transition:invalidation][transition:revalidation]") {
    auto observations = std::vector<ReferenceObservation>{};
    const auto select = node(
        [](In<TS<i64>> selection, In<REF<TS<i64>>, passive> first,
           In<REF<TS<i64>>, passive> second, In<REF<TS<i64>>, passive> third,
           Out<REF<TS<i64>>> output) {
            output.copy(*selection == 0 ? first : *selection == 1 ? second : third);
        });
    const auto observe = node([&](In<TS<i64>, optional> input) {
        observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                input.valid() ? *input : i64{}, input.last_updated()});
    });
    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<i64>> third,
                           wp<TS<i64>> selection) {
        observe(select(selection, first, second, third));
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time},
                  bg::input<TS<i64>>({10, literal::INVALID, literal::SKIP, literal::SKIP, 12, literal::INVALID}),
                  bg::input<TS<i64>>({literal::INVALID, literal::SKIP, literal::SKIP, 20, literal::SKIP,
                                      literal::SKIP}),
                  bg::input<TS<i64>>({40, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP,
                                      literal::SKIP}),
                  bg::input<TS<i64>>({0, 2, 1, 2, 0, 1}));

    const auto time = [&](int cycle) { return start_time + std::chrono::microseconds{cycle - 1}; };
    const auto expected = std::vector<ReferenceObservation>{
        {true, true, true, false, 10, time(1)},
        {true, true, false, false, 40, time(2)},
        {false, true, false, true, 0, time(3)},
        {true, true, true, false, 40, time(4)},
        {true, true, false, false, 12, time(5)},
        {true, true, false, false, 20, time(6)},
    };
    REQUIRE(observations.size() == expected.size());
    for (auto index = std::size_t{}; index < expected.size(); ++index) {
        const auto actual_time = observations[index].last_updated.time_since_epoch().count();
        const auto expected_time = expected[index].last_updated.time_since_epoch().count();
        CAPTURE(index);
        CAPTURE(actual_time, expected_time);
        CHECK(observations[index].valid == expected[index].valid);
        CHECK(observations[index].modified == expected[index].modified);
        CHECK(observations[index].became_valid == expected[index].became_valid);
        CHECK(observations[index].became_invalid == expected[index].became_invalid);
        CHECK(observations[index].value == expected[index].value);
        CHECK(observations[index].last_updated == expected[index].last_updated);
    }
}

TEST_CASE("reference outputs retain independent cycle-start validity across rebinding",
          "[evaluation][reference][sampling][validity][feature:reference][series:value]"
          "[transition:invalidation][transition:revalidation]") {
    struct repeated_reference_route {
        In<TS<i64>> step;
        In<REF<TS<i64>>, passive, optional> initial;
        In<REF<TS<i64>>, passive, optional> invalid;
        In<REF<TS<i64>>, passive, optional> final;
        Out<REF<TS<i64>>> output;

        void evaluate() const {
            if (*step == 0) {
                output.copy(initial);
            } else if (*step == 1) {
                output.copy(invalid);
            } else {
                output.copy(final);
            }
        }
    };
    struct value_target {
        i64 value;
        Out<TS<i64>> output;

        void start() const { output.set(value); }
    };
    struct invalid_target_a {
        Out<TS<i64>> output;

        void evaluate() const {}
    };
    struct invalid_target_b {
        Out<TS<i64>> output;

        void evaluate() const {}
    };

    auto previously_valid = std::vector<ReferenceObservation>{};
    auto previously_invalid = std::vector<ReferenceObservation>{};
    const auto route = node<repeated_reference_route>();
    const auto valid_source = node<value_target>();
    const auto empty_source_a = node<invalid_target_a>();
    const auto empty_source_b = node<invalid_target_b>();
    const auto observe = node([&](In<TS<i64>, optional> was_valid, In<TS<i64>, optional> was_invalid) {
        const auto observation = [](const auto &input) {
            return ReferenceObservation{input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                        input.valid() ? *input : i64{}, input.last_updated()};
        };
        previously_valid.push_back(observation(was_valid));
        previously_invalid.push_back(observation(was_invalid));
    });
    const auto graph = [&](wp<TS<i64>> step) {
        const auto valid = valid_source(10);
        const auto invalid_a = empty_source_a();
        const auto invalid_b = empty_source_b();
        const auto final = valid_source(30);
        observe(route(step, valid, invalid_b, final), route(step, invalid_a, invalid_b, final));
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time}, std::array{i64{0}, i64{1}, i64{2}});

    REQUIRE(previously_valid.size() == 3);
    REQUIRE(previously_invalid.size() == 3);
    CHECK(previously_valid[0].valid);
    CHECK(previously_valid[0].became_valid);
    CHECK_FALSE(previously_invalid[0].valid);
    CHECK_FALSE(previously_invalid[0].became_valid);
    CHECK_FALSE(previously_valid[1].valid);
    CHECK(previously_valid[1].became_invalid);
    CHECK_FALSE(previously_invalid[1].valid);
    CHECK_FALSE(previously_invalid[1].became_valid);
    CHECK_FALSE(previously_invalid[1].became_invalid);
    CHECK(previously_valid[1].modified);
    CHECK(previously_invalid[1].modified);
    CHECK(previously_valid[1].last_updated == start_time + std::chrono::microseconds{1});
    CHECK(previously_invalid[1].last_updated == start_time + std::chrono::microseconds{1});
    CHECK(previously_valid[2].valid);
    CHECK(previously_valid[2].became_valid);
    CHECK(previously_invalid[2].valid);
    CHECK(previously_invalid[2].became_valid);
    CHECK(previously_valid[2].value == 30);
    CHECK(previously_invalid[2].value == 30);
}

TEST_CASE("a newly added reference dictionary item observes the binding time",
          "[evaluation][reference][sampling][tsd][feature:reference][series:tsd][series:value]"
          "[binding:reference-to-value][transition:addition][transition:update]") {
    auto observations = std::vector<ReferenceObservation>{};
    auto source_times = std::vector<EvaluationTime>{};
    auto previous_key = i64{};
    const auto route = node([&](In<TS<i64>> key, In<REF<TS<i64>>, passive> value,
                                Out<TSD<i64, REF<TS<i64>>>> output) {
        if (*key == previous_key) {
            return;
        }
        if (previous_key != 0) {
            output.erase(previous_key);
        }
        output[*key].copy(value);
        previous_key = *key;
    });
    const auto observe = node([&](In<TSD<i64, TS<i64>>> input, In<TS<i64>, passive> source) {
        if (!input.contains(2)) {
            return;
        }
        const auto item = input[2];
        observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                item.valid() ? *item : i64{}, item.last_updated()});
        source_times.push_back(source.last_updated());
    });
    const auto graph = [&](wp<TS<i64>> value, wp<TS<i64>> key) {
        observe(route(key, value), value);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time},
                  bg::input<TS<i64>>({10, 11, literal::SKIP}), bg::input<TS<i64>>({1, 1, 2}));

    REQUIRE(observations.size() == 1);
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].became_valid);
    CHECK_FALSE(observations[0].became_invalid);
    CHECK(observations[0].value == 11);
    CHECK(observations[0].last_updated == start_time + std::chrono::microseconds{2});
    CHECK(source_times[0] == start_time + std::chrono::microseconds{1});
}

TEST_CASE("a graph-first dictionary of references propagates target invalidation and revalidation",
          "[evaluation][reference][tsd][validity][series:tsd][series:value][transition:addition]"
          "[transition:invalidation][transition:revalidation][binding:reference-to-value][feature:reference]") {
    struct store_reference {
        In<REF<TS<i64>>> input;
        Out<TSD<i64, REF<TS<i64>>>> output;

        void start() const { output[1].copy(input); }
    };

    auto observations = std::vector<ReferenceObservation>{};
    const auto target = node<invalidating_then_revalidating_reference_target>();
    const auto store = node<store_reference>();
    const auto observe = node([&](In<TSD<i64, TS<i64>>, optional> input) {
        const auto item = input[1];
        observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                item.valid() ? *item : i64{}, item.last_updated()});
    });
    const auto graph = [&] { observe(store(target())); };
    const auto start_time = MIN_DT + std::chrono::microseconds{1};

    bg::run_graph(graph, GraphRunOptions{.start_time = start_time});

    REQUIRE(observations.size() == 3);
    CHECK(observations[0] == ReferenceObservation{true, true, true, false, 10, start_time});
    CHECK(observations[1] == ReferenceObservation{false, true, false, true, 0,
                                                  start_time + std::chrono::microseconds{1}});
    CHECK(observations[2] == ReferenceObservation{true, true, true, false, 20,
                                                  start_time + std::chrono::microseconds{2}});
}

TEST_CASE("a passive dictionary of references reads the latest target state on a later trigger",
          "[evaluation][reference][tsd][validity][activity:passive][series:tsd][series:value]"
          "[transition:invalidation][transition:revalidation][binding:reference-to-value][feature:reference]") {
    struct store_reference {
        In<REF<TS<i64>>> input;
        Out<TSD<i64, REF<TS<i64>>>> output;

        void start() const { output[1].copy(input); }
    };

    auto observations = std::vector<ReferenceObservation>{};
    const auto target = node<delayed_revalidating_reference_target>();
    const auto store = node<store_reference>();
    const auto observe = node([&](In<TSD<i64, TS<i64>>, passive, optional> input, In<TS<i64>> trigger) {
        const auto item = input[1];
        observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                item.valid() ? *item : i64{}, item.last_updated()});
    });
    const auto graph = [&](wp<TS<i64>> trigger) { observe(store(target()), trigger); };
    const auto start_time = MIN_DT + std::chrono::microseconds{1};

    bg::run_graph(graph, GraphRunOptions{.start_time = start_time},
                  bg::input<TS<i64>>({1, literal::SKIP, 2, literal::SKIP, 3}));

    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].modified);
    CHECK(observations[0].became_valid);
    CHECK(observations[0].value == 10);
    CHECK(observations[1].valid == false);
    CHECK_FALSE(observations[1].modified);
    CHECK_FALSE(observations[1].became_invalid);
    CHECK(observations[1].last_updated == start_time + std::chrono::microseconds{1});
    CHECK(observations[2].valid);
    CHECK_FALSE(observations[2].modified);
    CHECK(observations[2].value == 20);
    CHECK_FALSE(observations[2].became_valid);
    CHECK(observations[2].last_updated == start_time + std::chrono::microseconds{3});
}

TEST_CASE("a reference dictionary item replacement, removal, and re-addition keep item history current",
          "[evaluation][reference][tsd][validity][series:tsd][series:value]"
          "[transition:update][transition:removal][transition:revalidation][feature:reference]") {
    struct route_reference {
        In<TS<i64>> action;
        In<REF<TS<i64>>, passive> first;
        In<REF<TS<i64>>, passive> second;
        Out<TSD<i64, REF<TS<i64>>>> output;

        void start() const { output[1].copy(first); }

        void evaluate() const {
            if (*action == 1) {
                output[1].copy(second);
            } else if (*action == 2) {
                output.erase(1);
            } else if (*action == 3) {
                output[1].copy(first);
            }
        }
    };
    struct DictionaryObservation {
        bool contains;
        ReferenceObservation item;
    };

    auto observations = std::vector<DictionaryObservation>{};
    const auto first_source = node([](Out<TS<i64>> output) { output.set(10); });
    const auto second_source = node([](Out<TS<i64>> output) { output.set(20); });
    const auto route = node<route_reference>();
    const auto observe = node([&](In<TSD<i64, TS<i64>>, optional> input) {
        if (input.contains(1)) {
            const auto item = input[1];
            observations.push_back({true, {item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                           item.valid() ? *item : i64{}, item.last_updated()}});
            return;
        }

        const auto key = i64{1};
        const auto id = input.find_removed_id(Scalar<i64>::from(key));
        REQUIRE(id != util::occupancy_index::npos);
        const auto item = input.item_at(id);
        observations.push_back({false, {item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                        item.valid() ? *item : i64{}, item.last_updated()}});
    });
    const auto graph = [&](wp<TS<i64>> action) {
        observe(route(action, first_source(), second_source()));
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time}, std::array{i64{0}, i64{1}, i64{2}, i64{3}});

    REQUIRE(observations.size() == 4);
    CHECK(observations[0].contains);
    CHECK(observations[0].item == ReferenceObservation{true, true, true, false, 10, start_time});
    CHECK(observations[1].contains);
    CHECK(observations[1].item == ReferenceObservation{true, true, false, false, 20,
                                                       start_time + std::chrono::microseconds{1}});
    CHECK_FALSE(observations[2].contains);
    CHECK_FALSE(observations[2].item.valid);
    CHECK_FALSE(observations[2].item.modified);
    CHECK(observations[3].contains);
    CHECK(observations[3].item == ReferenceObservation{true, true, true, false, 10,
                                                       start_time + std::chrono::microseconds{3}});
}

TEST_CASE("an adapted reference to a list of references observes the target item state",
          "[evaluation][reference][sampling][tsl][validity][feature:reference]"
          "[series:fixed-tsl][binding:reference-to-value][transition:revalidation]") {
    auto raw_list_states = std::vector<std::pair<bool, bool>>{};
    auto dereferenced_list_states = std::vector<std::pair<bool, bool>>{};
    auto item_observations = std::vector<ReferenceObservation>{};
    const auto invalid_source = node([](Out<TS<i64>>) {});
    const auto valid_source = node<sampled_right_source>();
    const auto make_list = node([](In<REF<TS<i64>>> item, Out<TSL<REF<TS<i64>>, 1>> output) {
        output[0].copy(item);
    });
    const auto select = node(
        [](In<TS<i64>> index, In<REF<TSL<REF<TS<i64>>, 1>>, passive> invalid,
           In<REF<TSL<REF<TS<i64>>, 1>>, passive> valid, Out<REF<TSL<REF<TS<i64>>, 1>>> output) {
            output.copy(*index == 0 ? invalid : valid);
        });
    const auto observe_raw = node([&](In<TSL<REF<TS<i64>>, 1>> input) {
        raw_list_states.emplace_back(input.valid(), input.became_valid());
    });
    const auto observe_item = node([&](In<TSL<TS<i64>, 1>, optional> input) {
        dereferenced_list_states.emplace_back(input.valid(), input.became_valid());
        const auto item = input[0];
        item_observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                     item.valid() ? *item : i64{}, item.last_updated()});
    });
    const auto graph = [&](wp<TS<i64>> index) {
        const auto invalid_list = make_list(invalid_source());
        const auto valid_list = make_list(valid_source());
        const auto selected = select(index, invalid_list, valid_list);
        observe_raw(selected);
        observe_item(selected);
    };

    bg::run_graph(graph, std::array{i64{0}, i64{1}});

    REQUIRE_FALSE(raw_list_states.empty());
    REQUIRE(raw_list_states.size() == 2);
    CHECK(raw_list_states[0] == std::pair{true, true});
    CHECK(raw_list_states[1] == std::pair{true, false});
    REQUIRE(dereferenced_list_states.size() == 2);
    CHECK(dereferenced_list_states[0] == std::pair{false, false});
    CHECK(dereferenced_list_states[1] == std::pair{true, true});
    REQUIRE(item_observations.size() == 2);
    CHECK_FALSE(item_observations[0].valid);
    CHECK_FALSE(item_observations[0].became_valid);
    CHECK(item_observations[1].valid);
    CHECK(item_observations[1].became_valid);
    CHECK_FALSE(item_observations[1].became_invalid);
    CHECK(item_observations[1].value == 20);
}

TEST_CASE("a reference input switches from direct to adapted and back to direct storage",
          "[evaluation][reference][sampling][tsl][validity][feature:reference]"
          "[series:fixed-tsl][transition:invalidation][transition:revalidation]") {
    struct Observation {
        bool valid;
        bool modified;
        bool became_valid;
        bool became_invalid;
        i64 value;
        EvaluationTime last_updated;
        const TimeSeriesRuntimeType *runtime_type;
        EvaluationTime first_source_time;
        EvaluationTime final_source_time;

        bool operator==(const Observation &) const = default;
    };

    auto observations = std::vector<Observation>{};
    auto child_observations = std::vector<ReferenceObservation>{};
    auto reference_item_observations = std::vector<ReferenceObservation>{};
    const auto first_source = node<sampled_left_source>();
    const auto invalid_source = node([](Out<TS<i64>>) {});
    const auto final_source = node([](Out<TS<i64>> output) { output.set(30); });
    const auto make_reference_list = node([](In<REF<TS<i64>>> item,
                                             Out<TSL<REF<TS<i64>>, 1>> output) {
        output[0].copy(item);
    });
    const auto select = node(
        [](In<TS<i64>> selection, In<REF<TSL<TS<i64>, 1>>, passive, optional> first,
           In<REF<TSL<REF<TS<i64>>, 1>>, passive, optional> adapted,
           In<REF<TSL<TS<i64>, 1>>, passive, optional> final,
           Out<REF<TSL<TS<i64>, 1>>> output) {
            if (*selection == 0) {
                output.copy(first);
            } else if (*selection == 1) {
                output.copy(adapted);
            } else {
                output.copy(final);
            }
        });
    const auto observe = node([&](In<TSL<TS<i64>, 1>, optional> input,
                                  In<TS<i64>, passive, optional> first_source,
                                  In<TS<i64>, passive, optional> final_source) {
        const auto item = input[0];
        observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                item.valid() ? *item : i64{}, input.last_updated(), &input.runtime_type(),
                                first_source.last_updated(), final_source.last_updated()});
        child_observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                      item.valid() ? *item : i64{}, item.last_updated()});
    });
    const auto observe_reference_item = node([&](In<TSL<REF<TS<i64>>, 1>, optional> input) {
        const auto item = input[0];
        reference_item_observations.push_back({item.valid(), item.modified(), item.became_valid(),
                                               item.became_invalid(), 0, item.last_updated()});
    });
    const auto graph = [&](wp<TS<i64>> selection) {
        const auto first = first_source();
        const auto first_list = tsl(first);
        const auto adapted_list = make_reference_list(invalid_source());
        const auto final = final_source();
        const auto final_list = tsl(final);
        const auto selected = select(selection, first_list, adapted_list, final_list);
        observe(selected, first, final);
        observe_reference_item(selected);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time}, std::array{i64{0}, i64{1}, i64{2}});

    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].became_valid);
    CHECK(observations[0].value == 10);
    CHECK(observations[1].became_invalid);
    CHECK_FALSE(observations[1].valid);
    CHECK(observations[2].became_valid);
    CHECK(observations[2].valid);
    CHECK(observations[2].value == 30);
    REQUIRE(child_observations.size() == 3);
    CHECK(child_observations[0].valid);
    CHECK(child_observations[0].value == 10);
    CHECK(child_observations[1].became_invalid);
    CHECK_FALSE(child_observations[1].valid);
    CHECK(child_observations[2].became_valid);
    CHECK(child_observations[2].valid);
    CHECK(child_observations[2].value == 30);
    REQUIRE(reference_item_observations.size() == 3);
    const auto expected_reference_item_time = [&](std::size_t index) {
        return start_time + std::chrono::microseconds{index};
    };
    CHECK(reference_item_observations[0].valid);
    CHECK(reference_item_observations[0].modified);
    CHECK(reference_item_observations[0].became_valid);
    CHECK(reference_item_observations[0].last_updated == expected_reference_item_time(0));
    CHECK(reference_item_observations[1].valid);
    CHECK(reference_item_observations[1].modified);
    CHECK_FALSE(reference_item_observations[1].became_valid);
    CHECK_FALSE(reference_item_observations[1].became_invalid);
    CHECK(reference_item_observations[1].last_updated == expected_reference_item_time(1));
    CHECK(reference_item_observations[2].valid);
    CHECK(reference_item_observations[2].modified);
    CHECK_FALSE(reference_item_observations[2].became_valid);
    CHECK_FALSE(reference_item_observations[2].became_invalid);
    CHECK(reference_item_observations[2].last_updated == expected_reference_item_time(2));
    CHECK(observations[0].runtime_type != observations[1].runtime_type);
    CHECK(observations[1].runtime_type != observations[2].runtime_type);
    for (auto index = std::size_t{}; index < observations.size(); ++index) {
        const auto expected_time = start_time + std::chrono::microseconds{index};
        const auto observed_time = observations[index].last_updated.time_since_epoch().count();
        const auto expected_time_count = expected_time.time_since_epoch().count();
        CAPTURE(index, observed_time, expected_time_count);
        CHECK(observations[index].modified);
        CHECK(observations[index].last_updated == expected_time);
        CHECK(observations[index].first_source_time == start_time);
        CHECK(observations[index].final_source_time == start_time);
    }
}

TEST_CASE("a passive dereferenced input observes its current target",
          "[evaluation][reference][activity][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][activity:passive]") {
    const auto left = node<sampled_left_source>();

    const auto right = node<sampled_right_source>();

    const auto select = node<reference_selector>();

    auto observations = std::vector<ReferenceObservation>{};
    const auto value = node([&](In<TS<i64>, passive, optional> input, In<TS<i64>> trigger,
                                Out<TS<i64>> output) {
        (void)trigger;
        observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                input.valid() ? *input : i64{}, input.last_updated()});
        output.set(*input);
    });
    const auto graph = [&](wp<TS<i64>> selection, wp<TS<i64>> trigger) {
        return value(select(selection, left(), right()), trigger);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto actual = bg::run_graph(graph, GraphRunOptions{.start_time = start_time},
                                      bg::input<TS<i64>>({0, 1, literal::SKIP}),
                                      bg::input<TS<i64>>({0, 1, 2}));

    CHECK(actual.equals({10, 20, 20}));
    REQUIRE(observations.size() == 3);
    CHECK(observations[0].valid);
    CHECK(observations[0].became_valid);
    CHECK(observations[0].value == 10);
    CHECK(observations[1].valid);
    CHECK_FALSE(observations[1].became_valid);
    CHECK_FALSE(observations[1].became_invalid);
    CHECK(observations[1].value == 20);
    CHECK_FALSE(observations[2].modified);
    CHECK_FALSE(observations[2].became_valid);
    CHECK_FALSE(observations[2].became_invalid);
    CHECK(observations[2].value == 20);
    CHECK(observations[2].last_updated == observations[1].last_updated);
}

TEST_CASE("a dynamic dereferenced input rebinds while passive",
          "[feature:input-activity][feature:reference][series:value][binding:reference-to-value]"
          "[definition:aggregate][activity:passive][transition:update]") {
    const auto left = node<sampled_left_source>();

    const auto right = node<sampled_right_source>();

    const auto condition = node<sampled_condition_source>();

    const auto select = node<reference_selector>();

    const auto value = node<dynamic_reference_value>();
    const auto graph = [&] {
        const auto trigger = condition();
        return value(select(trigger, left(), right()), trigger);
    };

    CHECK(bg::run_graph(graph).equals({10, 20}));
}

TEST_CASE("dynamic reference input unbinding reports one invalidation cycle",
          "[evaluation][reference][validity][feature:reference][transition:invalidation]"
          "[binding:reference-to-value][series:value][series:reference][guarantee:ordinary]") {
    const auto unbind = node<dynamic_reference_unbinder>();

    const auto graph = [&](wp<TSL<TS<i64>, 1>> collection,
                           wp<TS<i64>> trigger) {
        return unbind(collection, trigger);
    };

    CHECK(bg::run_graph(graph, {{{0, 10}}, {{0, 20}}}, {0, 1}).equals({true, false}));
}

namespace {

struct invalid_reference_target {
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct required_reference_forward {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("changing a reference to an invalid target invalidates required consumers",
          "[evaluation][reference][validity][feature:reference][validity:required]"
          "[transition:invalidation][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][series:value]") {
    const auto left = node<sampled_left_source>();

    const auto right = node<invalid_reference_target>();

    const auto condition = node<sampled_condition_source>();

    const auto select = node<reference_selector>();

    const auto forward = node<required_reference_forward>();
    const auto graph = [&] { return forward(select(condition(), left(), right())); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({10, std::nullopt}));
}

namespace {

struct list_reference_pass {
    In<REF<TSL<TS<i64>, 2>>> input;
    Out<REF<TSL<TS<i64>, 2>>> output;

    void evaluate() const { output.copy(input); }
};

struct referenced_list_value {
    In<TSL<TS<i64>, 2>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input[0]); }
};

struct generic_list_reference_pass {
    In<REF<TSL<TS<SCALAR>, 2>>> input;
    Out<REF<TSL<TS<SCALAR>, 2>>> output;

    void evaluate() const { output.copy(input); }
};

struct generic_referenced_list_value {
    In<TSL<TS<SCALAR>, 2>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*scalar_cast<i64>(input[0].value_view())); }
};

struct reference_list {
    In<REF<TS<i64>>> input;
    Out<TSL<REF<TS<i64>>, 1>> output;

    void evaluate() const { output[0].copy(input); }
};

struct repeated_value_target {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(20); }
};

struct delayed_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 3) {
            output.set(20);
            return;
        }

        ++*step;
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct revalidating_reference_list {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TS<i64>>, passive, optional> first;
    In<REF<TS<i64>>, passive, optional> second;
    State<int> step;
    Out<TSL<REF<TS<i64>>, 2>> output;

    void start(EvaluationContext *context) const {
        output[0].copy(first);
        output[1].copy(second);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
        } else if (*step == 1) {
            output[1].copy(second);
        } else {
            output[0].copy(first);
        }

        ++*step;
        if (*step < 3) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

struct stable_set_target {
    Out<TSS<i64>> output;

    void start() const {
        output.insert(1);
        output.insert(2);
    }

    void evaluate() const {}
};

struct delayed_invalidating_set_target {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSS<i64>> output;

    void start(EvaluationContext *context) const {
        output.insert(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            ++*step;
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.insert(1);
    }
};

struct revalidating_set_reference_list {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TSS<i64>>, passive, optional> first;
    In<REF<TSS<i64>>, passive, optional> second;
    State<int> step;
    Out<TSL<REF<TSS<i64>>, 2>> output;

    void start(EvaluationContext *context) const {
        output[0].copy(first);
        output[1].copy(second);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
        } else if (*step == 1) {
            output[1].copy(second);
        } else {
            output[0].copy(first);
        }

        ++*step;
        if (*step < 3) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

struct native_reference_bundle_source {
    In<TS<i64>> value;
    In<REF<TS<i64>>> selected;
    Out<TSB<NativeReferenceBundle>> output;

    void evaluate() const {
        output->value.copy(value);
        output->selected.copy(selected);
    }
};

struct reference_list_copy {
    In<TS<i64>> trigger;
    In<REF<TS<i64>>> input;
    Out<TSL<REF<TS<i64>>, 1>> output;

    void evaluate() const {
        (void)trigger;
        output[0].copy(input);
    }
};

struct reference_list_observer {
    In<TSL<REF<TS<i64>>, 1>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a native bundle adapts direct and referenced fields through its reference-free schema",
          "[evaluation][reference][tsb][feature:reference][series:tsb][transition:update]"
          "[transition:invalidation][transition:revalidation][representation:native]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto target = node<changing_reference_target>();
    const auto make_bundle = node<native_reference_bundle_source>();
    auto evaluations = 0;
    const auto observe = node([&](In<TS_T> series, Out<TS<i64>> output) {
        ++evaluations;
        const auto input = series.as<TSB<SCHEMA>>();
        const auto value = input[0].as<TS<i64>>();
        const auto selected = input[1].as<TS<i64>>();
        if (!selected.valid()) {
            output.invalidate();
            return;
        }
        output.set(*value * 100 + *selected);
    });
    const auto graph = [&](wp<TS<i64>> value) {
        const auto bundle = make_bundle(value, target());
        return observe(bundle.as<REF<TSB<SCHEMA>>>());
    };

    CHECK(bg::run_graph(graph, {1, 2, 3, 4}).equals({110, 220, std::nullopt, 430}));
    CHECK(evaluations == 4);
}

TEST_CASE("a reference to a bundle keeps the validity of each sampled field",
          "[evaluation][reference][sampling][tsb][validity][feature:reference]"
          "[series:tsb][transition:revalidation][binding:reference-to-value]") {
    struct bundle_reference_selector {
        In<TS<i64>> index;
        In<REF<TSB<SCHEMA>>, passive, optional> first;
        In<REF<TSB<SCHEMA>>, passive, optional> second;
        Out<REF<TSB<SCHEMA>>> output;

        void evaluate() const { output.copy(*index == 0 ? first : second); }
    };
    struct value_target {
        i64 value;
        Out<TS<i64>> output;

        void start() const { output.set(value); }
    };
    struct invalid_target {
        Out<TS<i64>> output;

        void evaluate() const {}
    };

    auto selected_states = std::vector<ReferenceObservation>{};
    auto value_states = std::vector<bool>{};
    const auto make_bundle = node<native_reference_bundle_source>();
    const auto select = node<bundle_reference_selector>();
    const auto value_source = node<value_target>();
    const auto invalid_source = node<invalid_target>();
    const auto observe = node([&](In<TS_T> series, Out<TS<i64>> output) {
        const auto input = series.as<TSB<SCHEMA>>();
        const auto value = input[0].as<TS<i64>>();
        const auto selected = input[1].as<TS<i64>>();
        value_states.push_back(value.valid());
        selected_states.push_back({selected.valid(), selected.modified(), selected.became_valid(),
                                   selected.became_invalid(), selected.valid() ? *selected : i64{},
                                   selected.last_updated()});
        output.set(*value);
    });
    const auto graph = [&](wp<TS<i64>> index) {
        const auto invalid = invalid_source();
        const auto valid = value_source(20);
        const auto first_value = value_source(100);
        const auto second_value = value_source(200);
        const auto first = make_bundle(first_value, invalid).as<REF<TSB<SCHEMA>>>();
        const auto second = make_bundle(second_value, valid).as<REF<TSB<SCHEMA>>>();
        return observe(select(index, first, second));
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    CHECK(bg::run_graph(graph, GraphRunOptions{.start_time = start_time}, std::array{i64{0}, i64{1}})
              .equals({100, 200}));
    const auto expected = std::vector<ReferenceObservation>{
        {false, true, false, false, 0, start_time},
        {true, true, true, false, 20, start_time + std::chrono::microseconds{1}},
    };
    REQUIRE(selected_states.size() == expected.size());
    for (auto index = std::size_t{}; index < expected.size(); ++index) {
        const auto actual_time = selected_states[index].last_updated.time_since_epoch().count();
        const auto expected_time = expected[index].last_updated.time_since_epoch().count();
        CAPTURE(index, actual_time, expected_time);
        CHECK(selected_states[index].valid == expected[index].valid);
        CHECK(selected_states[index].modified == expected[index].modified);
        CHECK(selected_states[index].became_valid == expected[index].became_valid);
        CHECK(selected_states[index].became_invalid == expected[index].became_invalid);
        CHECK(selected_states[index].value == expected[index].value);
        CHECK(selected_states[index].last_updated == expected[index].last_updated);
    }
    CHECK(value_states == std::vector<bool>{true, true});
}

TEST_CASE("sampling invalid bundle references updates a nested fixed-list item",
          "[evaluation][reference][sampling][tsb][tsl][validity][feature:reference]"
          "[series:fixed-tsl][transition:update][binding:reference-to-value]") {
    struct bundle_reference_selector {
        In<TS<i64>> index;
        In<REF<TSB<NestedReferenceBundle>>, passive, optional> first;
        In<REF<TSB<NestedReferenceBundle>>, passive, optional> second;
        Out<REF<TSB<NestedReferenceBundle>>> output;

        void evaluate() const { output.copy(*index == 0 ? first : second); }
    };

    auto observations = std::vector<ReferenceObservation>{};
    const auto first_source = node([](Out<TSB<NestedReferenceBundle>>) {});
    const auto second_source = node([](Out<TSB<NestedReferenceBundle>>) {});
    const auto select = node<bundle_reference_selector>();
    const auto observe = node([&](In<TSB<NestedReferenceBundle>, optional> input) {
        const auto item = input->values[0];
        observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(), 0,
                                item.last_updated()});
    });
    const auto graph = [&](wp<TS<i64>> index) {
        const auto first = first_source().as<REF<TSB<NestedReferenceBundle>>>();
        const auto second = second_source().as<REF<TSB<NestedReferenceBundle>>>();
        observe(select(index, first, second));
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time}, std::array{i64{0}, i64{1}});

    REQUIRE(observations.size() == 2);
    for (auto index = std::size_t{}; index < observations.size(); ++index) {
        const auto expected_time = start_time + std::chrono::microseconds{static_cast<i64>(index)};
        CAPTURE(index);
        CHECK_FALSE(observations[index].valid);
        CHECK(observations[index].modified);
        CHECK_FALSE(observations[index].became_valid);
        CHECK_FALSE(observations[index].became_invalid);
        CHECK(observations[index].last_updated == expected_time);
    }
}

TEST_CASE("a revalidated fixed-list reference resubscribes to the same target",
          "[evaluation][reference][feature:reference][tsl][series:fixed-tsl]"
          "[transition:invalidation]"
          "[transition:revalidation][transition:update][binding:reference-to-value]"
          "[representation:native]") {
    const auto target = node<delayed_reference_target>();
    const auto other = node([](Out<TS<i64>> output) { output.set(99); });
    const auto change = node<revalidating_reference_list>();
    const auto observe = node([](In<TSL<TS<i64>, 2>, optional> input, Out<TS<i64>> output) {
        const auto first = input[0];
        output.set(input.valid() && first.valid() ? *first : -1);
    });
    const auto graph = [&] {
        const auto collection = change(target(), other());
        return observe(collection.as<REF<TSL<TS<i64>, 2>>>());
    };

    CHECK(bg::run_graph(graph).equals({10, -1, -1, 10, 20}));
}

TEST_CASE("a revalidated fixed-list reference samples an unchanged TSS target as a first update",
          "[evaluation][reference][tsl][series:fixed-tsl][series:tss][feature:sampling]"
          "[transition:invalidation][transition:revalidation][binding:reference-to-value]"
          "[representation:native]") {
    const auto target = node<stable_set_target>();
    const auto change = node<revalidating_set_reference_list>();
    const auto observe = node([](In<TSL<TSS<i64>, 2>, optional> input, Out<TS<i64>> output) {
        if (!input.valid() || !input[0].valid()) {
            output.set(-1);
            return;
        }
        const auto key = i64{1};
        output.set(input[0].was_added(Scalar<i64>::from(key)) ? 1 : 0);
    });
    const auto graph = [&] {
        const auto collection = change(target(), target());
        return observe(collection.as<REF<TSL<TSS<i64>, 2>>>());
    };

    CHECK(bg::run_graph(graph).equals({1, -1, -1, 1}));
}

TEST_CASE("a nested fixed-TSL reference propagates delayed TSS invalidation and revalidation",
          "[evaluation][reference][tsl][series:fixed-tsl][series:tss][feature:sampling]"
          "[transition:invalidation][transition:revalidation][binding:value-to-reference]"
          "[binding:reference-to-value][representation:native]") {
    const auto target = node<delayed_invalidating_set_target>();
    const auto make_inner = node([](In<REF<TSS<i64>>> child, Out<TSL<REF<TSS<i64>>, 1>> output) {
        output[0].copy(child);
    });
    const auto observe = node([](In<TSL<TSL<TSS<i64>, 1>, 1>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid() && input[0].valid() && input[0][0].valid());
    });
    const auto graph = [&] {
        const auto inner = make_inner(target());
        const auto nested = tsl(inner).as<REF<TSL<TSL<REF<TSS<i64>>, 1>, 1>>>();
        return observe(nested);
    };

    CHECK(bg::run_graph(graph).equals({true, false, true}));
}

TEST_CASE("a dereferenced input subscribes directly to a fixed collection item",
          "[evaluation][reference][tsl][feature:reference][series:fixed-tsl][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = node<reference_pass>();

    const auto first = node(
        [](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) { output.set(*input[0]); });

    const auto graph = [&](wp<TSL<TS<i64>, 2>> source) { return pass(first(source)); };
    reference_pass::evaluation_count = 0;

    const auto actual = bg::run_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a fixed collection can pass through a reference",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = node<list_reference_pass>();

    const auto value = node<referenced_list_value>();

    const auto graph = [&](wp<TSL<TS<i64>, 2>> source) { return value(pass(source)); };

    const auto actual = bg::run_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a generic fixed collection input uses the referenced target runtime_type",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][declaration:generic]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = node<generic_list_reference_pass>();

    const auto value = node<generic_referenced_list_value>();

    const auto graph = [&](wp<TSL<TS<i64>, 2>> source) { return value(pass(source)); };

    const auto actual = bg::run_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(actual.equals({10, 20}));
}

namespace {

struct owned_pair_definition {
    In<TS<i64>> first;
    In<TS<i64>> second;
    Out<TSL<TS<i64>, 2>> output;

    void evaluate() const {
        output[0].set(*first);
        output[1].set(*second);
    }
};

struct pair_reference_selector_definition {
    In<TS<i64>> condition;
    In<REF<TSL<TS<i64>, 2>>, passive, optional> owned;
    In<REF<TSL<TS<i64>, 2>>, passive, optional> free;
    Out<REF<TSL<TS<i64>, 2>>> output;

    void evaluate() const { *condition == 0 ? output.copy(owned) : output.copy(free); }
};

struct pair_sum_definition {
    In<TSL<TS<i64>, 2>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

struct nested_reference_list_selector {
    In<TS<i64>> condition;
    In<REF<TS<i64>>, passive, optional> left;
    In<REF<TS<i64>>, passive, optional> right;
    Out<TSL<TSL<REF<TS<i64>>, 1>, 1>> output;

    void evaluate() const { output[0][0].copy(*condition == 0 ? left : right); }
};

struct invalidating_nested_reference_list {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TS<i64>>, passive, optional> first;
    In<REF<TS<i64>>, passive, optional> second;
    State<int> step;
    Out<TSL<TSL<REF<TS<i64>>, 2>, 1>> output;

    void start(EvaluationContext *context) const {
        output[0][0].copy(first);
        output[0][1].copy(second);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output[0][0].invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output[0][0].copy(first);
        }
        ++*step;
    }
};

struct reference_pair {
    In<REF<TS<i64>>> first;
    In<REF<TS<i64>>> second;
    Out<TSL<REF<TS<i64>>, 2>> output;

    void evaluate() const {
        output[0].copy(first);
        output[1].copy(second);
    }
};

} // namespace

TEST_CASE("a dereferenced list reference selects the current target runtime_type",
          "[evaluation][reference][tsl][free][feature:reference][series:fixed-tsl][representation:free]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto owned_pair = node<owned_pair_definition>();

    const auto select = node<pair_reference_selector_definition>();

    const auto sum = node<pair_sum_definition>();

    const auto graph = [&](wp<TS<i64>> condition, wp<TS<i64>> first_owned,
                           wp<TS<i64>> second_owned, wp<TS<i64>> first_free_item,
                           wp<TS<i64>> second_free_item) {
        const auto owned = owned_pair(first_owned, second_owned);
        const auto free = tsl(first_free_item, second_free_item);
        return sum(select(condition, owned, free));
    };
    const auto condition = std::vector{0, 1, 0};
    const auto first_owned = std::vector{1, 3, 5};
    const auto second_owned = std::vector{2, 4, 6};
    const auto first_free_item = std::vector{10, 20, 30};
    const auto second_free_item = std::vector{20, 40, 60};

    const auto result =
        bg::run_graph(graph, condition, first_owned, second_owned, first_free_item, second_free_item);

    CHECK(result.equals({3, 60, 11}));
}

TEST_CASE("a reference stored in a fixed collection item updates its consumer",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto source = node<changing_reference_target>();

    const auto list = node<reference_list>();

    const auto first = node([](In<TSL<REF<TS<i64>>, 1>> input,
                                   Out<REF<TS<i64>>> output) { output.copy(input[0]); });
    const auto graph = [&] { return first(list(source())); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({10, 20, std::nullopt, 30}));
}

TEST_CASE("nested fixed collection adaptation propagates reference updates and validity",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[transition:invalidation][transition:revalidation][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][representation:free]") {
    const auto first = node<changing_reference_target>();
    const auto second = node<changing_reference_target>();
    const auto make_inner = node([](In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<TSL<REF<TS<i64>>, 2>> output) {
        output[0].copy(first);
        output[1].copy(second);
    });
    const auto sum = node([](In<TSL<TSL<TS<i64>, 2>, 1>> input, Out<TS<i64>> output) {
        if (!input[0].valid() || !input[0][0].valid() || !input[0][1].valid()) {
            output.invalidate();
            return;
        }
        output.set(*input[0][0] + *input[0][1]);
    });
    const auto graph = [&] {
        const auto inner = make_inner(first(), second());
        const auto outer = tsl(inner).as<REF<TSL<TSL<REF<TS<i64>>, 2>, 1>>>();
        return sum(outer);
    };

    CHECK(bg::run_graph(graph).equals({20, 40, std::nullopt, 60}));
}

TEST_CASE("nested fixed collection adaptation propagates a reference item retarget",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[binding:value-to-reference][binding:reference-to-value][representation:native]") {
    const auto left = node<sampled_left_source>();
    const auto right = node<sampled_right_source>();
    const auto condition = node<sampled_condition_source>();
    const auto select = node<nested_reference_list_selector>();
    auto observations = std::vector<ReferenceObservation>{};
    auto evaluations = 0;
    const auto observe = node([&](In<TSL<TSL<TS<i64>, 1>, 1>> input, Out<TS<i64>> output) {
        ++evaluations;
        const auto item = input[0][0];
        observations.push_back({item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                item.valid() ? *item : i64{}, item.last_updated()});
        output.set(*item);
    });
    const auto graph = [&] {
        const auto collection = select(condition(), left(), right());
        const auto reference = collection.as<REF<TSL<TSL<REF<TS<i64>>, 1>, 1>>>();
        return observe(reference);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    CHECK(bg::run_graph(graph, GraphRunOptions{.start_time = start_time}).equals({10, 20}));
    CHECK(evaluations == 2);
    REQUIRE(observations.size() == 2);
    CHECK(observations[0].valid);
    CHECK(observations[0].became_valid);
    CHECK(observations[0].value == 10);
    CHECK(observations[0].last_updated == start_time);
    CHECK(observations[1].valid);
    CHECK_FALSE(observations[1].became_valid);
    CHECK_FALSE(observations[1].became_invalid);
    CHECK(observations[1].modified);
    CHECK(observations[1].value == 20);
    CHECK(observations[1].last_updated == start_time + std::chrono::microseconds{1});
}

TEST_CASE("nested fixed collection adaptation propagates reference item validity",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference]"
          "[transition:invalidation][transition:revalidation][binding:value-to-reference]"
          "[binding:reference-to-value][representation:native]") {
    const auto first = node<sampled_left_source>();
    const auto second = node<sampled_right_source>();
    const auto change_validity = node<invalidating_nested_reference_list>();
    auto evaluations = 0;
    const auto observe = node([&](In<TSL<TSL<TS<i64>, 2>, 1>> input, Out<TS<i64>> output) {
        ++evaluations;
        const auto &pair = input[0];
        output.set(pair.all_valid() ? *pair[0] + *pair[1] : -1);
    });
    const auto graph = [&] {
        const auto collection = change_validity(first(), second());
        const auto reference = collection.as<REF<TSL<TSL<REF<TS<i64>>, 2>, 1>>>();
        return observe(reference);
    };

    CHECK(bg::run_graph(graph).equals({30, -1, 30}));
    CHECK(evaluations == 3);
}

TEST_CASE("a reference-adapted fixed collection keeps cycle-start validity during an item swap",
          "[evaluation][reference][tsl][validity][feature:reference][series:fixed-tsl]"
          "[transition:invalidation][transition:revalidation][binding:reference-to-value]") {
    struct reference_pair {
        In<REF<TS<i64>>> first;
        In<REF<TS<i64>>> second;
        Out<TSL<REF<TS<i64>>, 2>> output;

        void start() const {
            output[0].copy(first);
            output[1].copy(second);
        }
    };
    struct Observation {
        ReferenceObservation collection;
        ReferenceObservation first;
        ReferenceObservation second;
    };

    auto observations = std::vector<Observation>{};
    const auto first = node<invalidating_reference_target>();
    const auto second = node<revalidating_reference_target>();
    const auto pair = node<reference_pair>();
    const auto observe = node([&](In<TSL<TS<i64>, 2>, optional> input) {
        const auto first_item = input[0];
        const auto second_item = input[1];
        const auto observe_item = [](const auto &item) {
            return ReferenceObservation{item.valid(), item.modified(), item.became_valid(), item.became_invalid(),
                                        item.valid() ? *item : i64{}, item.last_updated()};
        };
        observations.push_back({
            {input.valid(), input.modified(), input.became_valid(), input.became_invalid(), 0, input.last_updated()},
            observe_item(first_item), observe_item(second_item)});
    });
    const auto graph = [&] { observe(pair(first(), second())); };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time});

    const auto final_observation = std::ranges::find_if(
        observations | std::views::reverse,
        [&](const auto &observation) {
            return observation.first.last_updated == start_time + std::chrono::microseconds{1};
        });
    REQUIRE(final_observation != observations.rend());
    CHECK(final_observation->collection.valid);
    CHECK(final_observation->collection.modified);
    CHECK_FALSE(final_observation->collection.became_valid);
    CHECK_FALSE(final_observation->collection.became_invalid);
    CHECK_FALSE(final_observation->first.valid);
    CHECK(final_observation->first.became_invalid);
    CHECK(final_observation->second.valid);
    CHECK(final_observation->second.became_valid);
}

TEST_CASE("a free fixed collection publishes one validity transition for same-cycle item changes",
          "[evaluation][tsl][validity][feature:validity][feature:collection-change][series:fixed-tsl]"
          "[representation:free]"
          "[transition:invalidation][transition:revalidation]") {
    auto observations = std::vector<ReferenceObservation>{};
    const auto first = node<invalidating_reference_target>();
    const auto second = node<revalidating_reference_target>();
    const auto observe = node([&](In<TSL<TS<i64>, 2>, optional> input) {
        observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(), 0,
                                input.last_updated()});
    });
    const auto graph = [&] { observe(tsl(first(), second())); };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    bg::run_graph(graph, GraphRunOptions{.start_time = start_time});

    const auto final_observation = std::ranges::find_if(
        observations | std::views::reverse, [&](const auto &observation) {
            return observation.last_updated == start_time + std::chrono::microseconds{1};
        });
    REQUIRE(final_observation != observations.rend());
    CHECK(final_observation->valid);
    CHECK(final_observation->modified);
    CHECK_FALSE(final_observation->became_valid);
    CHECK_FALSE(final_observation->became_invalid);
}

TEST_CASE("sampling an outer reference updates each nested fixed-list item",
          "[evaluation][reference][sampling][tsl][validity][feature:reference]"
          "[series:fixed-tsl][transition:invalidation][transition:revalidation]") {
    struct nested_reference_source {
        In<REF<TS<i64>>> first;
        In<REF<TS<i64>>> second;
        Out<TSL<REF<TS<i64>>, 2>> output;

        void start() const {
            output[0].copy(first);
            output[1].copy(second);
        }
    };
    struct invalid_target {
        Out<TS<i64>> output;

        void evaluate() const {}
    };
    struct nested_collection_selector {
        In<TS<i64>> index;
        In<REF<TSL<TSL<REF<TS<i64>>, 2>, 1>>, passive, optional> first;
        In<REF<TSL<TSL<REF<TS<i64>>, 2>, 1>>, passive, optional> second;
        Out<REF<TSL<TSL<REF<TS<i64>>, 2>, 1>>> output;

        void evaluate() const { output.copy(*index == 1 ? second : first); }
    };
    struct Observation {
        ReferenceObservation first;
        ReferenceObservation second;
    };

    auto observations = std::vector<Observation>{};
    const auto make_inner = node<nested_reference_source>();
    const auto select = node<nested_collection_selector>();
    const auto invalid = node<invalid_target>();
    const auto left = node<sampled_left_source>();
    const auto right = node<sampled_right_source>();
    const auto observe = node([&](In<TSL<TSL<TS<i64>, 2>, 1>> input, Out<TS<i64>> output) {
        const auto first = input[0][0];
        const auto second = input[0][1];
        observations.push_back({
            {first.valid(), first.modified(), first.became_valid(), first.became_invalid(),
             first.valid() ? *first : i64{}, first.last_updated()},
            {second.valid(), second.modified(), second.became_valid(), second.became_invalid(),
             second.valid() ? *second : i64{}, second.last_updated()},
        });
        output.set((first.valid() ? *first : i64{-1}) * 100 + (second.valid() ? *second : i64{-1}));
    });
    const auto graph = [&](wp<TS<i64>> index) {
        const auto first = tsl(make_inner(left(), invalid()));
        const auto second = tsl(make_inner(invalid(), right()));
        return observe(select(index, first, second));
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto actual = bg::run_graph(graph, GraphRunOptions{.start_time = start_time},
                                      std::array{i64{0}, i64{1}, i64{0}});
    CHECK(actual.equals({999, -80, 999}));
    const auto expected = std::vector<Observation>{
        {{true, true, true, false, 10, start_time},
         {false, true, false, false, 0, start_time}},
        {{false, true, false, true, 0, start_time + std::chrono::microseconds{1}},
         {true, true, true, false, 20, start_time + std::chrono::microseconds{1}}},
        {{true, true, true, false, 10, start_time + std::chrono::microseconds{2}},
         {false, true, false, true, 0, start_time + std::chrono::microseconds{2}}},
    };
    REQUIRE(observations.size() == expected.size());
    for (auto index = std::size_t{}; index < expected.size(); ++index) {
        const auto &actual_first = observations[index].first;
        const auto &expected_first = expected[index].first;
        const auto &actual_second = observations[index].second;
        const auto &expected_second = expected[index].second;
        const auto actual_first_time = actual_first.last_updated.time_since_epoch().count();
        const auto expected_first_time = expected_first.last_updated.time_since_epoch().count();
        const auto actual_second_time = actual_second.last_updated.time_since_epoch().count();
        const auto expected_second_time = expected_second.last_updated.time_since_epoch().count();
        CAPTURE(index);
        CAPTURE(actual_first_time, expected_first_time, actual_second_time, expected_second_time);
        CHECK(actual_first.valid == expected_first.valid);
        CHECK(actual_first.modified == expected_first.modified);
        CHECK(actual_first.became_valid == expected_first.became_valid);
        CHECK(actual_first.became_invalid == expected_first.became_invalid);
        CHECK(actual_first.value == expected_first.value);
        CHECK(actual_first.last_updated == expected_first.last_updated);
        CHECK(actual_second.valid == expected_second.valid);
        CHECK(actual_second.modified == expected_second.modified);
        CHECK(actual_second.became_valid == expected_second.became_valid);
        CHECK(actual_second.became_invalid == expected_second.became_invalid);
        CHECK(actual_second.value == expected_second.value);
        CHECK(actual_second.last_updated == expected_second.last_updated);
    }
}

TEST_CASE("same-cycle target changes reach a selected native fixed-list item",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[feature:getitem][binding:value-to-reference][binding:reference-to-value]"
          "[guarantee:deduplicated][representation:native]") {
    const auto first = node<repeated_value_target>();
    const auto second = node<repeated_value_target>();
    const auto references = node<reference_pair>();
    auto evaluations = 0;
    const auto read = node([&](In<TS<i64>> input, Out<TS<i64>> output) {
        ++evaluations;
        output.set(*input);
    });
    const auto graph = [&] {
        const auto collection = references(first(), second());
        const auto value_reference = collection.as<REF<TSL<TS<i64>, 2>>>();
        return read(value_reference[i64{1}]);
    };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({10, 20}));
    CHECK(evaluations == 2);
}

TEST_CASE("a fixed collection output suppresses an equal nested reference update",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[guarantee:deduplicated]") {
    const auto source = node<repeated_value_target>();

    const auto copy = node<reference_list_copy>();

    const auto observe = node<reference_list_observer>();
    reference_list_observer::evaluation_count = 0;

    run_graph([&] {
        const auto target = source();
        observe(copy(target, target));
    });

    CHECK(reference_list_observer::evaluation_count == 1);
}

namespace {

struct invalidating_reference {
    static constexpr bool uses_timed_scheduling = true;

    In<REF<TS<i64>>, passive, optional> target;
    State<int> step;
    Out<REF<TS<i64>>> output;

    void start(EvaluationContext *context) const {
        output.copy(target);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.copy(target);
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("reference invalidation unbinds and revalidation resamples consumers",
          "[evaluation][reference][validity][feature:reference][transition:invalidation]"
          "[transition:revalidation][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][series:value]") {
    const auto source = node<sampled_left_source>();

    const auto reference = node<invalidating_reference>();
    auto observations = std::vector<ReferenceObservation>{};
    const auto observe = node([&](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        observations.push_back({input.valid(), input.modified(), input.became_valid(), input.became_invalid(),
                                input.valid() ? *input : i64{}, input.last_updated()});
        if (input.valid()) {
            output.set(*input);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&] { return observe(reference(source())); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({10, std::nullopt, 10}));
    REQUIRE(observations.size() == 3);
    CHECK(observations[0].became_valid);
    CHECK(observations[1].became_invalid);
    CHECK(observations[2].became_valid);
}

TEST_CASE("an initially unbound TSL reference permits optional nested item access",
          "[evaluation][reference][validity][series:fixed-tsl][shape:nested]"
          "[binding:reference-to-value][transition:invalidation][feature:reference]") {
    auto observations = std::vector<std::array<bool, 7>>{};
    const auto unbound = node([](Out<REF<TSL<TSL<TS<i64>, 1>, 1>>>) {});
    const auto trigger = node([](Out<TS<i64>> output) { output.set(1); });
    const auto observe = node([&](In<TSL<TSL<TS<i64>, 1>, 1>, optional> input, In<TS<i64>>) {
        auto outer_updated_count = std::size_t{};
        auto inner_updated_count = std::size_t{};
        input.for_each_updated([&](auto) { ++outer_updated_count; });
        input[0].for_each_updated([&](auto) { ++inner_updated_count; });
        observations.push_back({input.valid(), input[0].valid(), input[0][0].valid(), input.item_updated(0),
                                input[0].item_updated(0), outer_updated_count == 0, inner_updated_count == 0});
    });
    const auto graph = [&] { observe(unbound(), trigger()); };

    bg::run_graph(graph);

    REQUIRE(observations.size() == 1);
    CHECK(observations[0] == std::array{false, false, false, false, false, true, true});
}

TEST_CASE("an initially unbound TSB reference permits optional field item access",
          "[evaluation][reference][validity][series:tsb][shape:nested]"
          "[binding:reference-to-value][transition:invalidation][feature:reference]") {
    auto observations = std::vector<std::array<bool, 5>>{};
    const auto unbound = node([](Out<REF<TSB<NestedReferenceBundle>>>) {});
    const auto trigger = node([](Out<TS<i64>> output) { output.set(1); });
    const auto observe = node([&](In<TSB<NestedReferenceBundle>, optional> input, In<TS<i64>>) {
        auto updated_count = std::size_t{};
        input.for_each_updated([&](auto) { ++updated_count; });
        observations.push_back({input.valid(), input->values[0].valid(), input.item_updated(0),
                                updated_count == 0, input->values.item_updated(0)});
    });
    const auto graph = [&] { observe(wp<TSB<NestedReferenceBundle>>{unbound()}, trigger()); };

    bg::run_graph(graph);

    REQUIRE(observations.size() == 1);
    CHECK(observations[0] == std::array{false, false, false, true, false});
}

TEST_CASE("unbound native fixed collections report no updated items",
          "[evaluation][validity][series:fixed-tsl][series:tsb][representation:native]"
          "[feature:time-series-view][feature:reference]") {
    const auto list = In<TSL<TS<i64>, 1>, optional>{initial_time_series_state, nullptr,
                                                    cpp_time_series_runtime_type<TSL<TS<i64>, 1>>()};
    const auto bundle = In<TSB<NestedReferenceBundle>, optional>{
        initial_time_series_state, nullptr, cpp_time_series_runtime_type<TSB<NestedReferenceBundle>>()};

    CHECK_FALSE(list.item_updated(0));
    auto list_updated_count = std::size_t{};
    list.for_each_updated([&](auto) { ++list_updated_count; });
    CHECK(list_updated_count == 0);

    CHECK_FALSE(bundle.item_updated(0));
    auto bundle_updated_count = std::size_t{};
    bundle.for_each_updated([&](auto) { ++bundle_updated_count; });
    CHECK(bundle_updated_count == 0);
}

TEST_CASE("a sampled TSS output includes a same-cycle change to the previous target",
          "[evaluation][reference][series:tss][feature:sampling][transition:update]") {
    const auto select = node([](In<TS<bool>> condition,
                                    In<REF<TSS<i64>>, passive> old_target,
                                    In<REF<TSS<i64>>, passive> new_target,
                                    Out<REF<TSS<i64>>> output) {
        output.copy(*condition ? old_target : new_target);
    });

    const auto graph = [&](wp<TS<bool>> condition, wp<TSS<i64>> old_target,
                           wp<TSS<i64>> new_target) {
        return select(condition, old_target, new_target);
    };

    const auto same_cycle_change = bg::run_graph(
        graph, bg::input<TS<bool>>({true, false}),
        bg::input<TSS<i64>>({{2}, {3, literal::REMOVED(2)}}),
        bg::input<TSS<i64>>({{3}, literal::SKIP}));
    CHECK(same_cycle_change.equals({{2}, {3, literal::REMOVED(2)}}));

    const auto stale_change = bg::run_graph(
        graph, bg::input<TS<bool>>({true, true, false}),
        bg::input<TSS<i64>>({{2}, {3, literal::REMOVED(2)}, literal::SKIP}),
        bg::input<TSS<i64>>({{3}, literal::SKIP, literal::SKIP}));
    CHECK(stale_change.equals({{2}, {3, literal::REMOVED(2)}, {}}));
}
