#include <bg/graph.hpp>
#include <bg/graph/dynamic_node_subscriber_storage.hpp>
#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
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

template <template <typename, auto...> typename Field> struct DictionaryBundle {
    Field<TSD<i64, TS<i64>>> dictionary;
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed target",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = node<changing_reference_target>();
    const auto pass = node<reference_pass>();
    reference_pass::evaluation_count = 0;
    const auto graph = [&] { return pass(source()); };

    const auto actual = testing::test_graph(graph);

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20, std::nullopt, 30}));
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
        const auto constant_value = operators::const_(2.0).as<TS<double, TimeSeriesGuarantee::const_>>();
        const auto selected = select(use_constant, final_value(), constant_value);
        const auto output = copy(selected);

        CHECK(&output.type() == &TS<double>::type());
        return output;
    };

    const auto actual = testing::test_graph(graph, std::array{false, true, false, true});

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

    const auto actual = testing::test_graph(graph, std::array{false, true, false, true});

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

    const auto actual = testing::test_graph(graph);

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
        const auto first = operators::const_(i64{4}).as<
            TS<i64, TimeSeriesGuarantee::const_>>();
        const auto second = operators::const_(i64{7}).as<
            TS<i64, TimeSeriesGuarantee::const_>>();
        const auto values = tsl(select(use_second, first, second));
        const auto &type = dynamic_cast<const TimeSeriesListType &>(values.type());
        CHECK(type.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return read(values);
    };

    const auto actual = testing::test_graph(graph, std::array{false, true, false, true});

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

    const auto actual = testing::test_graph(graph, std::array{false, true, false, true});

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

    const auto actual = testing::test_graph(graph, {{{1, 10}}, {{1, 10}}}, {{{2, 20}}, {{2, 20}}},
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

    const auto actual = testing::test_graph(
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

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
    CHECK(evaluations == 1);
}

TEST_CASE("reference provider access preserves lazy allocation",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
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

struct sampled_condition_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct reference_selector {
    In<TS<i64>> condition;
    In<REF<TS<i64>>, passive, optional> left;
    In<REF<TS<i64>>, passive, optional> right;
    Out<REF<TS<i64>>> output;

    void evaluate() const { *condition ? output.copy(right) : output.copy(left); }
};

struct passive_reference_value {
    In<TS<i64>, passive, optional> input;
    In<TS<i64>> trigger;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input); }
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

    const auto actual = testing::test_graph(graph, GraphRunOptions{.start_time = start_time});

    CHECK(actual.equals<Observation>({
        Observation{10, start_time, start_time},
        Observation{20, start_time + std::chrono::microseconds{1},
                    start_time + std::chrono::microseconds{1}},
    }));
}

TEST_CASE("a passive dereferenced input observes its current target",
          "[evaluation][reference][activity][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][activity:passive]") {
    const auto left = node<sampled_left_source>();
    const auto right = node<sampled_right_source>();
    const auto condition = node<sampled_condition_source>();
    const auto select = node<reference_selector>();
    const auto value = node<passive_reference_value>();
    const auto graph = [&] {
        const auto trigger = condition();
        return value(select(trigger, left(), right()), trigger);
    };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({10, 20}));
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

    CHECK(testing::test_graph(graph).equals({10, 20}));
}

TEST_CASE("dynamic reference input unbinding reports one invalidation cycle",
          "[evaluation][reference][validity][feature:reference][transition:invalidation]"
          "[binding:reference-to-value][series:value][series:reference][guarantee:ordinary]") {
    const auto unbind = node<dynamic_reference_unbinder>();
    const auto graph = [&](wp<TSL<TS<i64>, 1>> collection,
                           wp<TS<i64>> trigger) {
        return unbind(collection, trigger);
    };

    CHECK(testing::test_graph(graph, {{{0, 10}}, {{0, 20}}}, {0, 1}).equals({true, false}));
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

    const auto actual = testing::test_graph(graph);

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

TEST_CASE("a dereferenced input subscribes directly to a fixed collection item",
          "[evaluation][reference][tsl][feature:reference][series:fixed-tsl][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = node<reference_pass>();
    const auto first = node(
        [](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) { output.set(*input[0]); });
    const auto graph = [&](wp<TSL<TS<i64>, 2>> source) { return pass(first(source)); };
    reference_pass::evaluation_count = 0;

    const auto actual = testing::test_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a fixed collection can pass through a reference",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = node<list_reference_pass>();
    const auto value = node<referenced_list_value>();
    const auto graph = [&](wp<TSL<TS<i64>, 2>> source) { return value(pass(source)); };

    const auto actual = testing::test_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a generic fixed collection input uses the referenced target runtime_type",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][declaration:generic]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = node<generic_list_reference_pass>();
    const auto value = node<generic_referenced_list_value>();
    const auto graph = [&](wp<TSL<TS<i64>, 2>> source) { return value(pass(source)); };

    const auto actual = testing::test_graph(graph, {{{0, 10}}, {{0, 20}}});

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
        testing::test_graph(graph, condition, first_owned, second_owned, first_free_item, second_free_item);

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

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({10, 20, std::nullopt, 30}));
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
    const auto graph = [&] { return reference(source()); };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({10, std::nullopt, 10}));
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
        return select(condition, old_target, new_target).as<TSS<i64>>();
    };

    const auto same_cycle_change = testing::test_graph(
        graph, testing::input<TS<bool>>({true, false}),
        testing::input<TSS<i64>>({{2}, {3, literal::REMOVED(2)}}),
        testing::input<TSS<i64>>({{3}, literal::SKIP}));
    CHECK(same_cycle_change.equals({{2}, {3, literal::REMOVED(2)}}));

    const auto stale_change = testing::test_graph(
        graph, testing::input<TS<bool>>({true, true, false}),
        testing::input<TSS<i64>>({{2}, {3, literal::REMOVED(2)}, literal::SKIP}),
        testing::input<TSS<i64>>({{3}, literal::SKIP, literal::SKIP}));
    CHECK(stale_change.equals({{2}, {3, literal::REMOVED(2)}, {}}));
}
