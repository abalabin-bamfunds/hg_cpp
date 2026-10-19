#include <bg/graph.hpp>
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

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace {

using namespace bg::literals;

inline constexpr auto G = bg::guarantee_variable<"G">;

struct changing_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
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
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;

    void evaluate() const {
        output.copy(input);
        ++evaluation_count;
    }

    inline static int evaluation_count{};
};

struct optional_reference_consumer {
    bg::In<bg::TS<bg::i64>, bg::optional> input;

    void evaluate() const {}
};

template <template <typename, auto...> typename Field> struct DictionaryBundle {
    Field<bg::TSD<bg::i64, bg::TS<bg::i64>>> dictionary;
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed target",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<changing_reference_target>();
    const auto pass = bg::node<reference_pass>();
    reference_pass::evaluation_count = 0;
    const auto graph = [&] { return pass(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20, std::nullopt, 30}));
}

TEST_CASE("an ordinary reference can retarget a value input between stronger targets",
          "[feature:reference][feature:type-system][feature:test-graph][series:value][scalar:floating-point]"
          "[guarantee:ordinary][guarantee:final][guarantee:const][binding:reference-to-value]"
          "[shape:chain][transition:update][definition:function]") {
    const auto final_value = bg::node([](bg::Out<bg::TS<double, bg::TimeSeriesGuarantee::final>> output) {
        output.set(1.0);
    });
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_constant,
           bg::In<bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>> first,
           bg::In<bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>> second,
           bg::Out<bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>> output) {
            output.copy(*use_constant ? second : first);
        });
    const auto copy = bg::node(
        [](bg::In<bg::TS<double, bg::guarantee_variable<"G">>> input,
           bg::Out<bg::TS<double, bg::guarantee_variable<"G">>> output) {
            output.set(*input);
        });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_constant) {
        const auto constant_value = bg::operators::const_(2.0).as<bg::TS<double, bg::TimeSeriesGuarantee::const_>>();
        const auto selected = select(use_constant, final_value(), constant_value);
        const auto output = copy(selected);

        CHECK(&output.type() == &bg::TS<double>::type());
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals<double>({1.0, 2.0, 1.0, 2.0}));
}

TEST_CASE("a deduplicated reference retarget samples equal constant values",
          "[feature:reference][feature:type-system][feature:test-graph][series:value][scalar:floating-point]"
          "[guarantee:deduplicated][guarantee:const][binding:reference-to-value]"
          "[shape:chain][transition:update][definition:function]") {
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second,
           bg::In<bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::const_>,
                          bg::TimeSeriesGuarantee::deduplicated>> first,
           bg::In<bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::const_>,
                          bg::TimeSeriesGuarantee::deduplicated>> second,
           bg::Out<bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::const_>,
                           bg::TimeSeriesGuarantee::deduplicated>> output) {
            output.copy(*use_second ? second : first);
        });
    const auto first = bg::node(
        [](bg::Out<bg::TS<double, bg::TimeSeriesGuarantee::const_>> output) { output.set(4.0); });
    const auto second = bg::node(
        [](bg::Out<bg::TS<double, bg::TimeSeriesGuarantee::const_>> output) { output.set(4.0); });
    const auto read = bg::node([](bg::In<bg::TS<double, bg::guarantee_variable<"G">>> input) { return *input; });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second) {
        return read(select(use_second, first(), second()));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals<double>({4.0, 4.0, 4.0, 4.0}));
}

TEST_CASE("a dereferenced list keeps the target runtime type when only guarantees differ",
          "[evaluation][reference][tsl][feature:reference][series:fixed-tsl][guarantee:ordinary]"
          "[guarantee:final][binding:reference-to-value]") {
    const auto first = bg::node(
        [](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> output) { output.set(1); });
    const auto second = bg::node(
        [](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> output) { output.set(2); });
    const auto pass = bg::node(
        [](bg::In<bg::REF<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>>> input,
           bg::Out<bg::REF<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>>> output) {
            output.copy(input);
        });
    auto target_type = static_cast<const bg::TimeSeriesType *>(nullptr);
    auto received_type = static_cast<const bg::TimeSeriesType *>(nullptr);
    const auto read = bg::node([&](bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input) {
        received_type = &input.runtime_type().type();
        return *input[0] + *input[1];
    });
    const auto graph = [&] {
        const auto values = bg::tsl(first(), second());
        target_type = &values.type();
        return read(pass(values));
    };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<bg::i64>({3}));
    CHECK(received_type == target_type);
}

TEST_CASE("a materialized free TSL samples retargeted ordinary references",
          "[evaluation][reference][tsl][feature:reference][feature:test-graph][series:fixed-tsl]"
          "[guarantee:ordinary][guarantee:const][binding:reference-to-value][transition:update]") {
    const auto read = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64, G>, 1>> input) { return *input[0]; });
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second,
           bg::In<bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::ordinary>,
                  bg::passive> first,
           bg::In<bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::ordinary>,
                  bg::passive> second,
           bg::Out<bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::ordinary>>
               output) { output.copy(*use_second ? second : first); });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second) {
        const auto first = bg::operators::const_(bg::i64{4}).as<
            bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        const auto second = bg::operators::const_(bg::i64{7}).as<
            bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
        const auto values = bg::tsl(select(use_second, first, second));
        const auto &type = dynamic_cast<const bg::TimeSeriesListType &>(values.type());
        CHECK(type.item_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
        return read(values);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals({4, 7, 4, 7}));
}

TEST_CASE("an ordinary list reference samples the selected list with final items",
          "[evaluation][reference][feature:reference][series:fixed-tsl][guarantee:ordinary]"
          "[guarantee:final][binding:reference-to-value][activity:active][transition:update][shape:chain]") {
    const auto first_item = bg::node(
        [](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> output) { output.set(10); });
    const auto first_other_item = bg::node(
        [](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> output) { output.set(11); });
    const auto second_item = bg::node(
        [](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> output) { output.set(20); });
    const auto second_other_item = bg::node(
        [](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> output) { output.set(21); });
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second,
           bg::In<bg::REF<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>,
                          bg::TimeSeriesGuarantee::ordinary>> first,
           bg::In<bg::REF<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>,
                          bg::TimeSeriesGuarantee::ordinary>> second,
           bg::Out<bg::REF<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>,
                           bg::TimeSeriesGuarantee::ordinary>> output) {
            output.copy(*use_second ? second : first);
        });
    const auto read = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input) { return *input[0]; });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second) {
        const auto first = bg::tsl(first_item(), first_other_item());
        const auto second = bg::tsl(second_item(), second_other_item());
        return read(select(use_second, first, second));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true, false, true});

    CHECK(actual.equals<bg::i64>({10, 20, 10, 20}));
}

TEST_CASE("plain and notifying inputs share one sampled dictionary",
          "[evaluation][reference][series:tsd][series:tsb][feature:sampling][transition:update]"
          "[binding:reference-to-value]") {
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
           bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
           bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
            output.copy(*use_second ? second : first);
        });
    const auto store_reference = bg::node(
        [](bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> input,
           bg::Out<bg::TSD<bg::i64, bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>>> output) {
            output[0].copy(input);
        });
    const auto compare = bg::node(
        [](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> direct,
           bg::In<bg::TSB<DictionaryBundle>> packaged, bg::Out<bg::TS<bool>> output) {
            output.set(direct.data() == packaged->dictionary.data());
        });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second,
                           bg::wp<bg::TS<bool>> use_second) {
        const auto selected = store_reference(select(use_second, first, second));
        const auto item = selected[bg::i64{0}];
        return compare(item, bg::bundle<DictionaryBundle>(item));
    };

    const auto actual = bg::testing::test_graph(graph, {{{1, 10}}, {{1, 10}}}, {{{2, 20}}, {{2, 20}}},
                                                {false, true});

    CHECK(actual.equals({true, true}));
}

TEST_CASE("a nested graph releases its sampled reference change when it stops",
          "[evaluation][nested][reference][series:tsd][feature:sampling][feature:nested-graph]"
          "[transition:update][transition:stop][transition:restart][binding:reference-to-value]") {
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
           bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
           bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
            output.copy(*use_second ? second : first);
        });
    const auto inspect = bg::node(
        [](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input, bg::Out<bg::TS<bg::i64>> output) {
            auto added = 0;
            auto removed = 0;
            input.for_each_added([&](const auto &key) { added |= 1 << *key; });
            input.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
            output.set(added * 100 + removed * 10);
        });
    const auto combine = bg::node(
        [](bg::In<bg::TS<bg::i64>, bg::optional> inner, bg::In<bg::TS<bg::i64>> direct,
           bg::Out<bg::TS<bg::i64>> output) {
            output.set((inner.valid() ? *inner : 0) + *direct);
        });
    const auto inner = bg::graph(
        "name"_n = "inspect_sampled_dictionary",
        [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) -> bg::wp<bg::TS<bg::i64>> {
            return inspect(input);
        });
    const auto outer = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        return combine(bg::nested(inner, run, selected), inspect(selected));
    };

    const auto actual = bg::testing::test_graph(
        outer, {{{1, 10}}, literal::SKIP, literal::SKIP, literal::SKIP},
        {{{2, 20}}, literal::SKIP, literal::SKIP, literal::SKIP},
        {false, literal::SKIP, true, false}, {true, true, false, true});

    CHECK(actual.equals({400, std::nullopt, 420, 440}));
}

TEST_CASE("a default reference output does not schedule subscribers for the same reference",
          "[evaluation][reference][feature:reference][transition:update][series:value][scalar:integer]"
          "[guarantee:deduplicated][binding:value-to-reference][binding:reference-to-reference]") {
    const auto forward = bg::node([](bg::In<bg::TS<bg::i64>>, bg::In<bg::REF<bg::TS<bg::i64>>, bg::passive> input,
                                     bg::Out<bg::REF<bg::TS<bg::i64>>> output) { output.copy(input); });
    auto evaluations = 0;
    const auto sink = bg::node([&](bg::In<bg::REF<bg::TS<bg::i64>>>) { ++evaluations; });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        sink(forward(input, input));
        return input;
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
    CHECK(evaluations == 1);
}

TEST_CASE("reference provider access preserves lazy allocation",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<changing_reference_target>();
    const auto pass = bg::node<reference_pass>();
    const auto sink = bg::node<optional_reference_consumer>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        sink(pass(source()));
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(bg::MIN_DT + std::chrono::microseconds{1});
    runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{2});

    auto &root = *static_cast<bg::details::RootReference *>(runtime.node(1).state());
    CHECK(root.has_inline_subscriber());
    CHECK_FALSE(root.has_provider());

    const auto reference = bg::Reference{&root};
    CHECK(bg::details::ReferenceProvider::get({}) == nullptr);
    CHECK(bg::details::ReferenceProvider::get(reference) == nullptr);

    auto &provider = bg::details::ReferenceProvider::require(reference);
    CHECK(bg::details::ReferenceProvider::get(reference) == &provider);
    CHECK(root.has_provider());

    runtime.stop();
}

TEST_CASE("a dynamic input callback crosses a graph boundary immediately",
          "[evaluation][reference][feature:reference][shape:multiple][transition:update]") {
    auto wiring = bg::GraphWiring{};
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto source_global_state = bg::GlobalState{};
    auto source = bg::RuntimeGraph{compiled, source_global_state};
    auto receiver_global_state = bg::GlobalState{};
    auto receiver = bg::RuntimeGraph{compiled, receiver_global_state};
    const auto start_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto update_time = start_time + std::chrono::microseconds{1};
    source.start(start_time);
    receiver.start(start_time);
    source.evaluate(update_time);

    struct CallbackResult {
        bg::RuntimeGraph *graph{};
        bg::EvaluationTime time{bg::MIN_DT};
        bg::OutputChange change{};
        int calls{};
    } result;

    auto *subscribers = static_cast<bg::details::DynamicSubscribers *>(nullptr);
    auto &subscription = bg::details::DynamicSubscribers::ensure(subscribers, source.allocation());
    subscription.add(
        receiver, 0, reinterpret_cast<std::size_t>(&result), 0, false, false,
        [](bg::OutputChangeContext &context, std::size_t, std::size_t data, bg::OutputChange change) {
            auto &result = *reinterpret_cast<CallbackResult *>(data);
            result.graph = &context.runtime_graph();
            result.time = context.evaluation_time();
            result.change = change;
            ++result.calls;
        });

    source.notify_dynamic_subscribers(subscription, bg::OutputChange::update);

    CHECK(result.graph == &receiver);
    CHECK(result.time == update_time);
    CHECK(result.change == bg::OutputChange::update);
    CHECK(result.calls == 1);
    bg::details::DynamicSubscribers::release(subscribers);
}

TEST_CASE("a second root subscriber promotes inline storage",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][shape:fan-out]") {
    auto wiring = bg::GraphWiring{};
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    auto root = bg::details::RootReference{};
    root.owner_graph = &runtime;
    const auto reference = bg::Reference{&root};

    bg::details::ReferenceProvider::add_subscriber(reference, runtime, 0, 0, 0, true, false);

    CHECK(root.has_inline_subscriber());

    bg::details::ReferenceProvider::remove_subscriber(reference, runtime, 0);

    CHECK_FALSE(root.has_inline_subscriber());
    CHECK_FALSE(root.has_dynamic_subscribers());

    bg::details::ReferenceProvider::add_subscriber(reference, runtime, 0, 0, 0, true, false);
    bg::details::ReferenceProvider::add_subscriber(reference, runtime, 8, 0, 1, true, false);

    CHECK_FALSE(root.has_inline_subscriber());
    CHECK(root.has_dynamic_subscribers());
    auto subscriber_count = std::size_t{};
    root.dynamic_subscribers()->for_each_node_subscriber([&](const auto &) { ++subscriber_count; });
    CHECK(subscriber_count == 2);

    bg::details::ReferenceProvider::finalize_root(root, runtime.allocation());
}

namespace {

struct sampled_left_source {
    bg::Out<bg::TS<bg::i64>> output;
    void start() const { output.set(10); }
    void evaluate() const {}
};

struct sampled_right_source {
    bg::Out<bg::TS<bg::i64>> output;
    void start() const { output.set(20); }
    void evaluate() const {}
};

struct sampled_condition_source {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct reference_selector {
    bg::In<bg::TS<bg::i64>> condition;
    bg::In<bg::REF<bg::TS<bg::i64>>, bg::passive, bg::optional> left;
    bg::In<bg::REF<bg::TS<bg::i64>>, bg::passive, bg::optional> right;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;

    void evaluate() const { *condition ? output.copy(right) : output.copy(left); }
};

struct sampled_reference_sink {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate(bg::EvaluationContext *context) const {
        values.push_back(*input);
        sample_times.push_back(input.last_updated());
        evaluation_times.push_back(context->evaluation_time());
    }

    inline static std::vector<int> values;
    inline static std::vector<bg::EvaluationTime> sample_times;
    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

struct passive_reference_value {
    bg::In<bg::TS<bg::i64>, bg::passive, bg::optional> input;
    bg::In<bg::TS<bg::i64>> trigger;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input); }
};

struct dynamic_reference_value {
    bg::In<bg::TS<bg::i64>, bg::optional, bg::dynamic_activity> input;
    bg::In<bg::TS<bg::i64>> trigger;
    bg::Out<bg::TS<bg::i64>> output;

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
    bg::In<bg::REF<bg::TSL<bg::TS<bg::i64>, 1>>> collection;
    bg::In<bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::ordinary>, bg::dynamic_binding, bg::optional> input;
    bg::In<bg::TS<bg::i64>> trigger;
    bg::Out<bg::TS<bool>> output;

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
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<sampled_right_source>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto sink = bg::node<sampled_reference_sink>();
    sampled_reference_sink::values.clear();
    sampled_reference_sink::sample_times.clear();
    sampled_reference_sink::evaluation_times.clear();

    bg::run_graph([&] { sink(select(condition(), left(), right())); });

    CHECK(sampled_reference_sink::values == std::vector{10, 20});
    CHECK(sampled_reference_sink::sample_times == sampled_reference_sink::evaluation_times);
}

TEST_CASE("a passive dereferenced input observes its current target",
          "[evaluation][reference][activity][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][activity:passive]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<sampled_right_source>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto value = bg::node<passive_reference_value>();
    const auto graph = [&] {
        const auto trigger = condition();
        return value(select(trigger, left(), right()), trigger);
    };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a dynamic dereferenced input rebinds while passive",
          "[feature:input-activity][feature:reference][series:value][binding:reference-to-value]"
          "[definition:aggregate][activity:passive][transition:update]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<sampled_right_source>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto value = bg::node<dynamic_reference_value>();
    const auto graph = [&] {
        const auto trigger = condition();
        return value(select(trigger, left(), right()), trigger);
    };

    CHECK(bg::testing::test_graph(graph).equals({10, 20}));
}

TEST_CASE("dynamic reference input unbinding reports one invalidation cycle",
          "[evaluation][reference][validity][feature:reference][transition:invalidation]"
          "[binding:reference-to-value][series:value]") {
    const auto unbind = bg::node<dynamic_reference_unbinder>();
    const auto graph = [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 1>> collection,
                           bg::wp<bg::TS<bg::i64>> trigger) {
        return unbind(collection, trigger);
    };

    CHECK(bg::testing::test_graph(graph, {{{0, 10}}, {{0, 20}}}, {0, 1}).equals({true, false}));
}

namespace {

struct invalid_reference_target {
    bg::Out<bg::TS<bg::i64>> output;
    void evaluate() const {}
};

struct required_reference_forward {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("changing a reference to an invalid target invalidates required consumers",
          "[evaluation][reference][validity][feature:reference][validity:required]"
          "[transition:invalidation][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][series:value]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<invalid_reference_target>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto forward = bg::node<required_reference_forward>();
    const auto graph = [&] { return forward(select(condition(), left(), right())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, std::nullopt}));
}

namespace {

struct list_reference_pass {
    bg::In<bg::REF<bg::TSL<bg::TS<bg::i64>, 2>>> input;
    bg::Out<bg::REF<bg::TSL<bg::TS<bg::i64>, 2>>> output;

    void evaluate() const { output.copy(input); }
};

struct referenced_list_value {
    bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input[0]); }
};

struct generic_list_reference_pass {
    bg::In<bg::REF<bg::TSL<bg::TS<bg::SCALAR>, 2>>> input;
    bg::Out<bg::REF<bg::TSL<bg::TS<bg::SCALAR>, 2>>> output;

    void evaluate() const { output.copy(input); }
};

struct generic_referenced_list_value {
    bg::In<bg::TSL<bg::TS<bg::SCALAR>, 2>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*bg::scalar_cast<bg::i64>(input[0].value_view())); }
};

struct reference_list {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::TSL<bg::REF<bg::TS<bg::i64>>, 1>> output;

    void evaluate() const { output[0].copy(input); }
};

struct repeated_value_target {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(20); }
};

struct reference_list_copy {
    bg::In<bg::TS<bg::i64>> trigger;
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::TSL<bg::REF<bg::TS<bg::i64>>, 1>> output;

    void evaluate() const {
        (void)trigger;
        output[0].copy(input);
    }
};

struct reference_list_observer {
    bg::In<bg::TSL<bg::REF<bg::TS<bg::i64>>, 1>> input;

    void evaluate() const { ++evaluation_count; }

    inline static int evaluation_count{};
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed collection item",
          "[evaluation][reference][tsl][feature:reference][series:fixed-tsl][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = bg::node<reference_pass>();
    const auto first = bg::node(
        [](bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input, bg::Out<bg::TS<bg::i64>> output) { output.set(*input[0]); });
    const auto graph = [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> source) { return pass(first(source)); };
    reference_pass::evaluation_count = 0;

    const auto actual = bg::testing::test_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a fixed collection can pass through a reference",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = bg::node<list_reference_pass>();
    const auto value = bg::node<referenced_list_value>();
    const auto graph = [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> source) { return value(pass(source)); };

    const auto actual = bg::testing::test_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a generic fixed collection input uses the referenced target runtime_type",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][declaration:generic]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto pass = bg::node<generic_list_reference_pass>();
    const auto value = bg::node<generic_referenced_list_value>();
    const auto graph = [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> source) { return value(pass(source)); };

    const auto actual = bg::testing::test_graph(graph, {{{0, 10}}, {{0, 20}}});

    CHECK(actual.equals({10, 20}));
}

namespace {

struct owned_pair_definition {
    bg::In<bg::TS<bg::i64>> first;
    bg::In<bg::TS<bg::i64>> second;
    bg::Out<bg::TSL<bg::TS<bg::i64>, 2>> output;

    void evaluate() const {
        output[0].set(*first);
        output[1].set(*second);
    }
};

struct pair_reference_selector_definition {
    bg::In<bg::TS<bg::i64>> condition;
    bg::In<bg::REF<bg::TSL<bg::TS<bg::i64>, 2>>, bg::passive, bg::optional> owned;
    bg::In<bg::REF<bg::TSL<bg::TS<bg::i64>, 2>>, bg::passive, bg::optional> free;
    bg::Out<bg::REF<bg::TSL<bg::TS<bg::i64>, 2>>> output;

    void evaluate() const { *condition == 0 ? output.copy(owned) : output.copy(free); }
};

struct pair_sum_definition {
    bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

} // namespace

TEST_CASE("a dereferenced list reference selects the current target runtime_type",
          "[evaluation][reference][tsl][free][feature:reference][series:fixed-tsl][representation:free]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto owned_pair = bg::node<owned_pair_definition>();
    const auto select = bg::node<pair_reference_selector_definition>();
    const auto sum = bg::node<pair_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> condition, bg::wp<bg::TS<bg::i64>> first_owned,
                           bg::wp<bg::TS<bg::i64>> second_owned, bg::wp<bg::TS<bg::i64>> first_free_item,
                           bg::wp<bg::TS<bg::i64>> second_free_item) {
        const auto owned = owned_pair(first_owned, second_owned);
        const auto free = bg::tsl(first_free_item, second_free_item);
        return sum(select(condition, owned, free));
    };
    const auto condition = std::vector{0, 1, 0};
    const auto first_owned = std::vector{1, 3, 5};
    const auto second_owned = std::vector{2, 4, 6};
    const auto first_free_item = std::vector{10, 20, 30};
    const auto second_free_item = std::vector{20, 40, 60};

    const auto result =
        bg::testing::test_graph(graph, condition, first_owned, second_owned, first_free_item, second_free_item);

    CHECK(result.equals({3, 60, 11}));
}

TEST_CASE("a reference stored in a fixed collection item updates its consumer",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<changing_reference_target>();
    const auto list = bg::node<reference_list>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::REF<bg::TS<bg::i64>>, 1>> input,
                                   bg::Out<bg::REF<bg::TS<bg::i64>>> output) { output.copy(input[0]); });
    const auto graph = [&] { return first(list(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20, std::nullopt, 30}));
}

TEST_CASE("a fixed collection output suppresses an equal nested reference update",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[guarantee:deduplicated]") {
    const auto source = bg::node<repeated_value_target>();
    const auto copy = bg::node<reference_list_copy>();
    const auto observe = bg::node<reference_list_observer>();
    reference_list_observer::evaluation_count = 0;

    bg::run_graph([&] {
        const auto target = source();
        observe(copy(target, target));
    });

    CHECK(reference_list_observer::evaluation_count == 1);
}

namespace {

struct invalidating_reference {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<bg::REF<bg::TS<bg::i64>>, bg::passive, bg::optional> target;
    bg::State<int> step;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;

    void start(bg::EvaluationContext *context) const {
        output.copy(target);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
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
    const auto source = bg::node<sampled_left_source>();
    const auto reference = bg::node<invalidating_reference>();
    const auto graph = [&] { return reference(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, std::nullopt, 10}));
}

TEST_CASE("a sampled TSS output includes a same-cycle change to the previous target",
          "[evaluation][reference][series:tss][feature:sampling][transition:retarget][transition:update]") {
    const auto select = bg::node([](bg::In<bg::TS<bool>> condition,
                                    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive> old_target,
                                    bg::In<bg::REF<bg::TSS<bg::i64>>, bg::passive> new_target,
                                    bg::Out<bg::REF<bg::TSS<bg::i64>>> output) {
        output.copy(*condition ? old_target : new_target);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> condition, bg::wp<bg::TSS<bg::i64>> old_target,
                           bg::wp<bg::TSS<bg::i64>> new_target) {
        return select(condition, old_target, new_target).as<bg::TSS<bg::i64>>();
    };

    const auto same_cycle_change = bg::testing::test_graph(
        graph, bg::testing::input<bg::TS<bool>>({true, false}),
        bg::testing::input<bg::TSS<bg::i64>>({{2}, {3, literal::REMOVED(2)}}),
        bg::testing::input<bg::TSS<bg::i64>>({{3}, literal::SKIP}));
    CHECK(same_cycle_change.equals({{2}, {3, literal::REMOVED(2)}}));

    const auto stale_change = bg::testing::test_graph(
        graph, bg::testing::input<bg::TS<bool>>({true, true, false}),
        bg::testing::input<bg::TSS<bg::i64>>({{2}, {3, literal::REMOVED(2)}, literal::SKIP}),
        bg::testing::input<bg::TSS<bg::i64>>({{3}, literal::SKIP, literal::SKIP}));
    CHECK(stale_change.equals({{2}, {3, literal::REMOVED(2)}, {}}));
}
